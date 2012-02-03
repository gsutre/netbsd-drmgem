/*	$OpenBSD: sg_dma.c,v 1.9 2010/04/20 23:12:01 phessler Exp $	*/
/*
 * Copyright (c) 2009 Owain G. Ainsworth <oga@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * Copyright (c) 2003 Henric Jungheim
 * Copyright (c) 2001, 2002 Eduardo Horvath
 * Copyright (c) 1999, 2000 Matthew R. Green
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Support for scatter/gather style dma through agp or an iommu.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/extent.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/proc.h>

#include <sys/bus.h>
#include <machine/bus_private.h>
#include <machine/cpu.h>

#include <x86/sg_dma.h>

#ifndef MAX_DMA_SEGS
#define MAX_DMA_SEGS	20
#endif

/* Disable 24-bit DMA support if it's not supported by arch. */
#ifndef BUS_DMA_24BIT
#define BUS_DMA_24BIT	0
#endif

#ifndef _BUS_DMAMEM_ALLOC_RANGE
#define _BUS_DMAMEM_ALLOC_RANGE _bus_dmamem_alloc_range
#endif /* _BUS_DMAMEM_ALLOC_RANGE */

static int	sg_dmamap_create(void *, bus_dma_tag_t, bus_size_t, int,
		    bus_size_t, bus_size_t, int, bus_dmamap_t *);
static void	sg_dmamap_destroy(void *, bus_dma_tag_t, bus_dmamap_t);
static int	sg_dmamap_load(void *, bus_dma_tag_t, bus_dmamap_t, void *,
		    bus_size_t, struct proc *, int);
static int	sg_dmamap_load_mbuf(void *, bus_dma_tag_t, bus_dmamap_t,
		    struct mbuf *, int);
static int	sg_dmamap_load_uio(void *, bus_dma_tag_t, bus_dmamap_t,
		    struct uio *, int);
static int	sg_dmamap_load_raw(void *, bus_dma_tag_t, bus_dmamap_t,
		    bus_dma_segment_t *, int, bus_size_t, int);
static void	sg_dmamap_unload(void *, bus_dma_tag_t, bus_dmamap_t);

/*
static int	sg_dmamap_load_buffer(bus_dma_tag_t, bus_dmamap_t, void *,
		    bus_size_t, struct proc *, int, int *, int);
static int	sg_dmamap_load_physarray(bus_dma_tag_t, bus_dmamap_t,
		    paddr_t *, int, int, int *, int);
*/

static int	sg_dmamem_alloc(void *, bus_dma_tag_t, bus_size_t, bus_size_t,
		    bus_size_t, bus_dma_segment_t *, int, int *, int);

static int	sg_dmamap_load_seg(bus_dma_tag_t, struct sg_cookie *,
		    bus_dmamap_t, bus_dma_segment_t *, int, int, bus_size_t,
		    bus_size_t);

static struct sg_page_map *sg_iomap_create(int);
static int	sg_dmamap_append_range(bus_dma_tag_t, bus_dmamap_t, paddr_t,
		    bus_size_t, int, bus_size_t);
static int	sg_iomap_insert_page(struct sg_page_map *, paddr_t);
static bus_addr_t	sg_iomap_translate(struct sg_page_map *, paddr_t);
static void	sg_iomap_load_map(struct sg_cookie *, struct sg_page_map *,
		    bus_addr_t, int);
static void	sg_iomap_unload_map(struct sg_cookie *, struct sg_page_map *);
static void	sg_iomap_destroy(struct sg_page_map *);
static void	sg_iomap_clear_pages(struct sg_page_map *);

int
sg_dmatag_create(const char *name, void *hdl, bus_dma_tag_t odmat,
    bus_addr_t start, bus_size_t size,
    void bind(void *, bus_addr_t, paddr_t, int),
    void unbind(void *, bus_addr_t), void flushtlb(void *),
    void dmasync(void *, bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
	bus_size_t, int),
    bus_dma_tag_t *dmat)
{
	struct sg_cookie	*sg;
	uint64_t		 present;
	int			 error;

	if (bind == NULL || unbind == NULL || flushtlb == NULL)
		return (EINVAL);

	sg = kmem_zalloc(sizeof(*sg), KM_NOSLEEP);
	if (sg == NULL)
		return (ENOMEM);

	present =
	    BUS_DMAMAP_OVERRIDE_CREATE |
	    BUS_DMAMAP_OVERRIDE_DESTROY |
	    BUS_DMAMAP_OVERRIDE_LOAD |
	    BUS_DMAMAP_OVERRIDE_LOAD_MBUF |
	    BUS_DMAMAP_OVERRIDE_LOAD_UIO |
	    BUS_DMAMAP_OVERRIDE_LOAD_RAW |
	    BUS_DMAMAP_OVERRIDE_UNLOAD |
	    BUS_DMAMEM_OVERRIDE_ALLOC
	    /* XXX: BUS_DMAMEM_OVERRIDE_FREE */;

	sg->sg_ov.ov_dmamap_create = sg_dmamap_create;
	sg->sg_ov.ov_dmamap_destroy = sg_dmamap_destroy;
	sg->sg_ov.ov_dmamap_load = sg_dmamap_load;
	sg->sg_ov.ov_dmamap_load_mbuf = sg_dmamap_load_mbuf;
	sg->sg_ov.ov_dmamap_load_uio = sg_dmamap_load_uio;
	sg->sg_ov.ov_dmamap_load_raw = sg_dmamap_load_raw;
	sg->sg_ov.ov_dmamap_unload = sg_dmamap_unload;
	sg->sg_ov.ov_dmamem_alloc = sg_dmamem_alloc;

	if (dmasync != NULL) {
		present |= BUS_DMAMAP_OVERRIDE_SYNC;
		sg->sg_ov.ov_dmamap_sync = dmasync;
	}

	sg->sg_ex = extent_create(name, start, start + size - 1,
	    NULL, 0, EX_NOWAIT | EX_NOCOALESCE);
	if (sg->sg_ex == NULL) {
		kmem_free(sg, sizeof(*sg));
		return (ENOMEM);
	}

	error = bus_dma_tag_create(odmat, present, &sg->sg_ov, sg, dmat);
	if (error != 0) {
		extent_destroy(sg->sg_ex);
		kmem_free(sg, sizeof(*sg));
		return (error);
	}

#ifdef DIAGNOSTIC
	if ((*dmat)->_bounce_thresh != 0)
		printf("%s: bounce thresh %#"PRIxPADDR"\n", __func__,
		    (*dmat)->_bounce_thresh);
#endif

	sg->sg_hdl = hdl;
	sg->sg_dmat = odmat;
	mutex_init(&sg->sg_mtx, MUTEX_DEFAULT, IPL_HIGH);
	sg->bind_page = bind;
	sg->unbind_page = unbind;
	sg->flush_tlb = flushtlb;

	return (0);
}

void
sg_dmatag_destroy(struct sg_cookie *sg)
{
	mutex_destroy(&sg->sg_mtx);
	extent_destroy(sg->sg_ex);
	kmem_free(sg, sizeof(*sg));
}

static int
sg_dmamap_create(void *ctx, bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamap)
{
	struct sg_cookie	*sg = ctx;
	struct sg_page_map	*spm;
	bus_dmamap_t		 map;
	int			 ret;

	if ((ret = bus_dmamap_create(sg->sg_dmat, size, nsegments, maxsegsz, boundary,
	    flags, &map)) != 0)
		return (ret);

#ifdef DIAGNOSTIC
	if (map->_dm_cookie != NULL)
		printf("%s: non-NULL cookie, bounce thresh %#"PRIxPADDR"\n", __func__,
		    map->_dm_bounce_thresh);
#endif

	if ((spm = sg_iomap_create(atop(round_page(size)))) == NULL) {
		bus_dmamap_destroy(sg->sg_dmat, map);
		return (ENOMEM);
	}

	map->_dm_sg_cookie = spm;
	*dmamap = map;

	return (0);
}

void
sg_dmamap_set_alignment(bus_dma_tag_t tag, bus_dmamap_t dmam,
    u_long alignment)
{
	if (alignment < PAGE_SIZE)
		return;

	dmam->dm_segs[0]._ds_align = alignment;
}

static void
sg_dmamap_destroy(void *ctx, bus_dma_tag_t t, bus_dmamap_t map)
{
	struct sg_cookie	*sg = ctx;

	/*
	 * The specification (man page) requires a loaded
	 * map to be unloaded before it is destroyed.
	 */
	if (map->dm_nsegs)
		bus_dmamap_unload(t, map);

	if (map->_dm_sg_cookie)
		sg_iomap_destroy(map->_dm_sg_cookie);
	map->_dm_sg_cookie = NULL;
	bus_dmamap_destroy(sg->sg_dmat, map);
}

/*
 * Load a contiguous kva buffer into a dmamap.  The physical pages are
 * not assumed to be contiguous.  Two passes are made through the buffer
 * and both call pmap_extract() for the same va->pa translations.  It
 * is possible to run out of pa->dvma mappings; the code should be smart
 * enough to resize the iomap (when the "flags" permit allocation).  It
 * is trivial to compute the number of entries required (round the length
 * up to the page size and then divide by the page size)...
 */
static int
sg_dmamap_load(void *ctx, bus_dma_tag_t t, bus_dmamap_t map, void *buf,
    bus_size_t buflen, struct proc *p, int flags)
{
	int err = 0;
	bus_size_t sgsize;
	u_long dvmaddr, sgstart, sgend;
	bus_size_t align, boundary;
	struct sg_cookie *sg = ctx;
	struct sg_page_map *spm = map->_dm_sg_cookie;
	pmap_t pmap;

	if (map->dm_nsegs) {
		/*
		 * Is it still in use? _bus_dmamap_load should have taken care
		 * of this.
		 */
#ifdef DIAGNOSTIC
		panic("sg_dmamap_load: map still in use");
#endif
		bus_dmamap_unload(t, map);
	}

	/*
	 * Make sure that on error condition we return "no valid mappings".
	 */
	map->dm_nsegs = 0;

	if (buflen < 1 || buflen > map->_dm_size)
		return (EINVAL);

	/*
	 * A boundary presented to bus_dmamem_alloc() takes precedence
	 * over boundary in the map.
	 */
	if ((boundary = (map->dm_segs[0]._ds_boundary)) == 0)
		boundary = map->_dm_boundary;
	align = MAX(map->dm_segs[0]._ds_align, PAGE_SIZE);

	pmap = p ? p->p_vmspace->vm_map.pmap : pmap_kernel();

	/* Count up the total number of pages we need */
	sg_iomap_clear_pages(spm);
	{ /* Scope */
		bus_addr_t a, aend;
		bus_addr_t addr = (bus_addr_t)(vaddr_t)buf;
		int seg_len = buflen;

		aend = round_page(addr + seg_len);
		for (a = trunc_page(addr); a < aend; a += PAGE_SIZE) {
			paddr_t pa;

			if (pmap_extract(pmap, a, &pa) == FALSE) {
				printf("iomap pmap error addr %#"PRIxPADDR"\n", a);
				sg_iomap_clear_pages(spm);
				return (EFBIG);
			}

			err = sg_iomap_insert_page(spm, pa);
			if (err) {
				printf("iomap insert error: %d for "
				    "va %#"PRIxPADDR" pa %#"PRIxPADDR" "
				    "(buf %p len %zd/%zx)\n",
				    err, a, pa, buf, buflen, buflen);
				sg_iomap_clear_pages(spm);
				return (EFBIG);
			}
		}
	}
	sgsize = spm->spm_pagecnt * PAGE_SIZE;

	mutex_enter(&sg->sg_mtx);
	if (flags & BUS_DMA_24BIT) {
		sgstart = MAX(sg->sg_ex->ex_start, 0xff000000);
		sgend = MIN(sg->sg_ex->ex_end, 0xffffffff);
	} else {
		sgstart = sg->sg_ex->ex_start;
		sgend = sg->sg_ex->ex_end;
	}

	/*
	 * If our segment size is larger than the boundary we need to
	 * split the transfer up into little pieces ourselves.
	 */
	err = extent_alloc_subregion1(sg->sg_ex, sgstart, sgend,
	    sgsize, align, 0, (sgsize > boundary) ? 0 : boundary,
	    EX_NOWAIT | EX_BOUNDZERO, (u_long *)&dvmaddr);
	mutex_exit(&sg->sg_mtx);

	if (err != 0) {
#ifdef DIAGNOSTIC
		printf("%s: unable to allocate extent subregion: %d\n",
		    __func__, err);
#endif
		sg_iomap_clear_pages(spm);
		return (err);
	}

	/* Set the active DVMA map */
	spm->spm_start = dvmaddr;
	spm->spm_size = sgsize;

	map->dm_mapsize = buflen;

	sg_iomap_load_map(sg, spm, dvmaddr, flags);

	{ /* Scope */
		bus_addr_t a, aend;
		bus_addr_t addr = (bus_addr_t)(vaddr_t)buf;
		int seg_len = buflen;

		aend = round_page(addr + seg_len);
		for (a = trunc_page(addr); a < aend; a += PAGE_SIZE) {
			bus_addr_t pgstart;
			bus_addr_t pgend;
			paddr_t pa;
			int pglen;

			/* Yuck... Redoing the same pmap_extract... */
			if (pmap_extract(pmap, a, &pa) == FALSE) {
				printf("iomap pmap error addr %#"PRIxPADDR"\n", a);
				err = EFBIG;
				break;
			}

			pgstart = pa | (MAX(a, addr) & PAGE_MASK);
			pgend = pa | (MIN(a + PAGE_SIZE - 1,
			    addr + seg_len - 1) & PAGE_MASK);
			pglen = pgend - pgstart + 1;

			if (pglen < 1)
				continue;

			err = sg_dmamap_append_range(t, map, pgstart,
			    pglen, flags, boundary);
			if (err == EFBIG)
				break;
			else if (err) {
				printf("iomap load seg page: %d for "
				    "va %#"PRIxPADDR" pa %#"PRIxPADDR" (%#"PRIxPADDR" - %#"PRIxPADDR") "
				    "for %d/0x%x\n",
				    err, a, pa, pgstart, pgend, pglen, pglen);
				break;
			}
		}
	}
	if (err) {
		sg_dmamap_unload(ctx, t, map);
	} else {
		spm->spm_origbuf = buf;
		spm->spm_buftype = X86_DMA_BUFTYPE_LINEAR;
		spm->spm_proc = p;
	}

	return (err);
}

/*
 * Load an mbuf into our map. we convert it to some bus_dma_segment_ts then
 * pass it to load_raw.
 */
static int
sg_dmamap_load_mbuf(void *ctx, bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *mb,
    int flags)
{
	/*
	 * This code is adapted from sparc64, for very fragmented data
	 * we may need to adapt the algorithm
	 */
	bus_dma_segment_t	 segs[MAX_DMA_SEGS];
	struct sg_page_map	*spm = map->_dm_sg_cookie;
	size_t			 len;
	int			 i, err;

	/*
	 * Make sure that on error condition we return "no valid mappings".
	 */
	map->dm_mapsize = 0;
	map->dm_nsegs = 0;

	if (mb->m_pkthdr.len > map->_dm_size)
		return (EINVAL);

	i = 0;
	len = 0;
	while (mb) {
		vaddr_t	vaddr = mtod(mb, vaddr_t);
		long	buflen = (long)mb->m_len;

		len += buflen;
		while (buflen > 0 && i < MAX_DMA_SEGS) {
			paddr_t	pa;
			long incr;

			incr = min(buflen, NBPG);

			if (pmap_extract(pmap_kernel(), vaddr, &pa) == FALSE)
				return EINVAL;

			buflen -= incr;
			vaddr += incr;

			if (i > 0 && pa == (segs[i - 1].ds_addr +
			    segs[i - 1].ds_len) && ((segs[i - 1].ds_len + incr)
			    < map->_dm_maxmaxsegsz)) {
				/* contigious, great! */
				segs[i - 1].ds_len += incr;
				continue;
			}
			segs[i].ds_addr = pa;
			segs[i].ds_len = incr;
			segs[i]._ds_boundary = 0;
			segs[i]._ds_align = 0;
			i++;
		}
		mb = mb->m_next;
		if (mb && i >= MAX_DMA_SEGS) {
			/* our map, it is too big! */
			return (EFBIG);
		}
	}

	err = sg_dmamap_load_raw(ctx, t, map, segs, i, (bus_size_t)len, flags);

	if (err == 0) {
		spm->spm_origbuf = mb;
		spm->spm_buftype = X86_DMA_BUFTYPE_MBUF;
	}
	return (err);
}

/*
 * Load a uio into the map. Turn it into segments and call load_raw()
 */
static int
sg_dmamap_load_uio(void *ctx, bus_dma_tag_t t, bus_dmamap_t map, struct uio *uio,
    int flags)
{
	/*
	 * loading uios is kinda broken since we can't lock the pages.
	 * and unlock them at unload. Perhaps page loaning is the answer.
	 * 'till then we only accept kernel data
	 */
	bus_dma_segment_t	 segs[MAX_DMA_SEGS];
	struct sg_page_map	*spm = map->_dm_sg_cookie;
	size_t			 len;
	int			 i, j, err;

	/*
	 * Make sure that on errror we return "no valid mappings".
	 */
	map->dm_mapsize = 0;
	map->dm_nsegs = 0;

	if (uio->uio_resid > map->_dm_size)
		return (EINVAL);

	if (!VMSPACE_IS_KERNEL_P(uio->uio_vmspace))
		return (EOPNOTSUPP);

	i = j = 0;
	len = 0;
	while (j < uio->uio_iovcnt) {
		vaddr_t	vaddr = (vaddr_t)uio->uio_iov[j].iov_base;
		long	buflen = (long)uio->uio_iov[j].iov_len;

		len += buflen;
		while (buflen > 0 && i < MAX_DMA_SEGS) {
			paddr_t pa;
			long incr;

			incr = min(buflen, NBPG);
			(void)pmap_extract(pmap_kernel(), vaddr, &pa);
			buflen -= incr;
			vaddr += incr;

			if (i > 0 && pa == (segs[i - 1].ds_addr +
			    segs[i -1].ds_len) && ((segs[i - 1].ds_len + incr)
			    < map->_dm_maxmaxsegsz)) {
				/* contigious, yay! */
				segs[i - 1].ds_len += incr;
				continue;
			}
			segs[i].ds_addr = pa;
			segs[i].ds_len = incr;
			segs[i]._ds_boundary = 0;
			segs[i]._ds_align = 0;
			i++;
		}
		j++;
		if ((uio->uio_iovcnt - j) && i >= MAX_DMA_SEGS) {
			/* Our map, is it too big! */
			return (EFBIG);
		}

	}

	err = sg_dmamap_load_raw(ctx, t, map, segs, i, (bus_size_t)len, flags);

	if (err == 0) {
		spm->spm_origbuf = uio;
		spm->spm_buftype = X86_DMA_BUFTYPE_UIO;
	}
	return (err);
}

/*
 * Load a dvmamap from an array of segs.  It calls sg_dmamap_append_range()
 * or for part of the 2nd pass through the mapping.
 */
static int
sg_dmamap_load_raw(void *ctx, bus_dma_tag_t t, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, bus_size_t size, int flags)
{
	int i;
	int left;
	int err = 0;
	bus_size_t sgsize;
	bus_size_t boundary, align;
	u_long dvmaddr, sgstart, sgend;
	struct sg_cookie *sg = ctx;
	struct sg_page_map *spm = map->_dm_sg_cookie;

	if (map->dm_nsegs) {
		/* Already in use?? */
#ifdef DIAGNOSTIC
		panic("sg_dmamap_load_raw: map still in use");
#endif
		bus_dmamap_unload(t, map);
	}

	/*
	 * A boundary presented to bus_dmamem_alloc() takes precedence
	 * over boundary in the map.
	 */
	if ((boundary = segs[0]._ds_boundary) == 0)
		boundary = map->_dm_boundary;

	align = MAX(MAX(segs[0]._ds_align, map->dm_segs[0]._ds_align),
	    PAGE_SIZE);

	/*
	 * Make sure that on error condition we return "no valid mappings".
	 */
	map->dm_nsegs = 0;

	sg_iomap_clear_pages(spm);
	/* Count up the total number of pages we need */
	for (i = 0, left = size; left > 0 && i < nsegs; i++) {
		bus_addr_t a, aend;
		bus_size_t len = segs[i].ds_len;
		bus_addr_t addr = segs[i].ds_addr;
		int seg_len = MIN(left, len);

		if (len < 1)
			continue;

		aend = round_page(addr + seg_len);
		for (a = trunc_page(addr); a < aend; a += PAGE_SIZE) {

			err = sg_iomap_insert_page(spm, a);
			if (err) {
				printf("iomap insert error: %d for "
				    "pa %#"PRIxPADDR"\n", err, a);
				sg_iomap_clear_pages(spm);
				return (EFBIG);
			}
		}

		left -= seg_len;
	}
	sgsize = spm->spm_pagecnt * PAGE_SIZE;

	mutex_enter(&sg->sg_mtx);
	if (flags & BUS_DMA_24BIT) {
		sgstart = MAX(sg->sg_ex->ex_start, 0xff000000);
		sgend = MIN(sg->sg_ex->ex_end, 0xffffffff);
	} else {
		sgstart = sg->sg_ex->ex_start;
		sgend = sg->sg_ex->ex_end;
	}

	/*
	 * If our segment size is larger than the boundary we need to
	 * split the transfer up into little pieces ourselves.
	 */
	err = extent_alloc_subregion1(sg->sg_ex, sgstart, sgend,
	    sgsize, align, 0, (sgsize > boundary) ? 0 : boundary,
	    EX_NOWAIT | EX_BOUNDZERO, (u_long *)&dvmaddr);
	mutex_exit(&sg->sg_mtx);

	if (err != 0) {
#ifdef DIAGNOSTIC
		printf("%s: unable to allocate extent subregion: %d\n",
		    __func__, err);
#endif
		sg_iomap_clear_pages(spm);
		return (err);
	}

	/* Set the active DVMA map */
	spm->spm_start = dvmaddr;
	spm->spm_size = sgsize;

	map->dm_mapsize = size;

	sg_iomap_load_map(sg, spm, dvmaddr, flags);

	err = sg_dmamap_load_seg(t, sg, map, segs, nsegs, flags,
	    size, boundary);

	if (err) {
		sg_dmamap_unload(ctx, t, map);
	} else {
		/* This will be overwritten if mbuf or uio called us */
		spm->spm_origbuf = segs;
		spm->spm_buftype = X86_DMA_BUFTYPE_RAW;
	}

	return (err);
}

/*
 * Insert a range of addresses into a loaded map respecting the specified
 * boundary and alignment restrictions.  The range is specified by its
 * physical address and length.  The range cannot cross a page boundary.
 * This code (along with most of the rest of the function in this file)
 * assumes that the IOMMU page size is equal to PAGE_SIZE.
 */
static int
sg_dmamap_append_range(bus_dma_tag_t t, bus_dmamap_t map, paddr_t pa,
    bus_size_t length, int flags, bus_size_t boundary)
{
	struct sg_page_map *spm = map->_dm_sg_cookie;
	bus_addr_t sgstart, sgend, bd_mask;
	bus_dma_segment_t *seg = NULL;
	int i = map->dm_nsegs;

	sgstart = sg_iomap_translate(spm, pa);
	sgend = sgstart + length - 1;

#ifdef DIAGNOSTIC
	if (sgstart == 0 || sgstart > sgend) {
		printf("append range invalid mapping for %#"PRIxPADDR" "
		    "(%#"PRIxPADDR" - %#"PRIxPADDR")\n", pa, sgstart, sgend);
		map->dm_nsegs = 0;
		return (EINVAL);
	}
#endif

#ifdef DEBUG
	if (trunc_page(sgstart) != trunc_page(sgend)) {
		printf("append range crossing page boundary! "
		    "pa %#"PRIxPADDR" length %zd/0x%zx sgstart %#"PRIxPADDR" sgend %#"PRIxPADDR"\n",
		    pa, length, length, sgstart, sgend);
	}
#endif

	/*
	 * We will attempt to merge this range with the previous entry
	 * (if there is one).
	 */
	if (i > 0) {
		seg = &map->dm_segs[i - 1];
		if (sgstart == seg->ds_addr + seg->ds_len) {
			length += seg->ds_len;
			sgstart = seg->ds_addr;
			sgend = sgstart + length - 1;
		} else
			seg = NULL;
	}

	if (seg == NULL) {
		seg = &map->dm_segs[i];
		if (++i > map->_dm_segcnt) {
			map->dm_nsegs = 0;
#ifdef DIAGNOSTIC
			printf("%s: max segment count reached: %d > %d\n",
			    __func__, i, map->_dm_segcnt);
#endif
			return (EFBIG);
		}
	}

	/*
	 * At this point, "i" is the index of the *next* bus_dma_segment_t
	 * (the segment count, aka map->dm_nsegs) and "seg" points to the
	 * *current* entry.  "length", "sgstart", and "sgend" reflect what
	 * we intend to put in "*seg".  No assumptions should be made about
	 * the contents of "*seg".  Only "boundary" issue can change this
	 * and "boundary" is often zero, so explicitly test for that case
	 * (the test is strictly an optimization).
	 */
	if (boundary != 0) {
		bd_mask = ~(boundary - 1);

		while ((sgstart & bd_mask) != (sgend & bd_mask)) {
			/*
			 * We are crossing a boundary so fill in the current
			 * segment with as much as possible, then grab a new
			 * one.
			 */

			seg->ds_addr = sgstart;
			seg->ds_len = boundary - (sgstart & bd_mask);

			sgstart += seg->ds_len; /* sgend stays the same */
			length -= seg->ds_len;

			seg = &map->dm_segs[i];
			if (++i > map->_dm_segcnt) {
				map->dm_nsegs = 0;
#ifdef DIAGNOSTIC
				printf("%s: max segment count reached: %d > %d\n",
				    __func__, i, map->_dm_segcnt);
#endif
				return (EFBIG);
			}
		}
	}

	seg->ds_addr = sgstart;
	seg->ds_len = length;
	map->dm_nsegs = i;

	return (0);
}

/*
 * Populate the iomap from a bus_dma_segment_t array.  See note for
 * sg_dmamap_load() regarding page entry exhaustion of the iomap.
 * This is less of a problem for load_seg, as the number of pages
 * is usually similar to the number of segments (nsegs).
 */
static int
sg_dmamap_load_seg(bus_dma_tag_t t, struct sg_cookie *sg,
    bus_dmamap_t map, bus_dma_segment_t *segs, int nsegs, int flags,
    bus_size_t size, bus_size_t boundary)
{
	int i;
	int left;
	int seg;

	/*
	 * Keep in mind that each segment could span
	 * multiple pages and that these are not always
	 * adjacent. The code is no longer adding dvma
	 * aliases to the IOMMU.  The STC will not cross
	 * page boundaries anyway and a IOMMU table walk
	 * vs. what may be a streamed PCI DMA to a ring
	 * descriptor is probably a wash.  It eases TLB
	 * pressure and in the worst possible case, it is
	 * only as bad a non-IOMMUed architecture.  More
	 * importantly, the code is not quite as hairy.
	 * (It's bad enough as it is.)
	 */
	left = size;
	seg = 0;
	for (i = 0; left > 0 && i < nsegs; i++) {
		bus_addr_t a, aend;
		bus_size_t len = segs[i].ds_len;
		bus_addr_t addr = segs[i].ds_addr;
		int seg_len = MIN(left, len);

		if (len < 1)
			continue;

		aend = round_page(addr + seg_len);
		for (a = trunc_page(addr); a < aend; a += PAGE_SIZE) {
			bus_addr_t pgstart;
			bus_addr_t pgend;
			int pglen;
			int err;

			pgstart = MAX(a, addr);
			pgend = MIN(a + PAGE_SIZE - 1, addr + seg_len - 1);
			pglen = pgend - pgstart + 1;

			if (pglen < 1)
				continue;

			err = sg_dmamap_append_range(t, map, pgstart,
			    pglen, flags, boundary);
			if (err == EFBIG)
				return (err);
			if (err) {
				printf("iomap load seg page: %d for "
				    "pa %#"PRIxPADDR" (%#"PRIxPADDR" - %#"PRIxPADDR" for %d/%x\n",
				    err, a, pgstart, pgend, pglen, pglen);
				return (err);
			}

		}

		left -= seg_len;
	}
	return (0);
}

/*
 * Unload a dvmamap.
 */
static void
sg_dmamap_unload(void *ctx, bus_dma_tag_t t, bus_dmamap_t map)
{
	struct sg_cookie	*sg = ctx;
	struct sg_page_map	*spm = map->_dm_sg_cookie;
	bus_addr_t		 dvmaddr = spm->spm_start;
	bus_size_t		 sgsize = spm->spm_size;
	int			 error;

	/* Remove the IOMMU entries */
	sg_iomap_unload_map(sg, spm);

	/* Clear the iomap */
	sg_iomap_clear_pages(spm);

	mutex_enter(&sg->sg_mtx);
	error = extent_free(sg->sg_ex, dvmaddr,
		sgsize, EX_NOWAIT);
	spm->spm_start = 0;
	spm->spm_size = 0;
	mutex_exit(&sg->sg_mtx);
	if (error != 0)
		printf("warning: %zd of DVMA space lost\n", sgsize);

	spm->spm_buftype = X86_DMA_BUFTYPE_INVALID;
	spm->spm_origbuf = NULL;
	spm->spm_proc = NULL;
	bus_dmamap_unload(sg->sg_dmat, map);
}

/*
 * Alloc dma safe memory, telling the backend that we're scatter gather
 * to ease pressure on the vm.
 *
 * This assumes that we can map all physical memory.
 */
static int
sg_dmamem_alloc(void *ctx, bus_dma_tag_t t, bus_size_t size,
    bus_size_t alignment, bus_size_t boundary, bus_dma_segment_t *segs,
    int nsegs, int *rsegs, int flags)
{
	return (_BUS_DMAMEM_ALLOC_RANGE(t, size, alignment, boundary,
	    segs, nsegs, rsegs, flags | BUS_DMA_SG, 0, -1));
}

/*
 * Create a new iomap.
 */
static struct sg_page_map *
sg_iomap_create(int n)
{
	struct sg_page_map	*spm;

	/* Safety for heavily fragmented data, such as mbufs */
	n += 4;
	if (n < 16)
		n = 16;

	spm = malloc(sizeof(*spm) + (n - 1) * sizeof(spm->spm_map[0]),
		M_DMAMAP, M_NOWAIT | M_ZERO);
	if (spm == NULL) {
#ifdef DIAGNOSTIC
		printf("%s: unable to allocate page map with %d entries\n",
		    __func__, n);
#endif
		return (NULL);
	}

	/* Initialize the map. */
	spm->spm_maxpage = n;
	SPLAY_INIT(&spm->spm_tree);

	return (spm);
}

/*
 * Destroy an iomap.
 */
static void
sg_iomap_destroy(struct sg_page_map *spm)
{
#ifdef DIAGNOSTIC
	if (spm->spm_pagecnt > 0)
		printf("%s: %d page entries in use\n", __func__,
		    spm->spm_pagecnt);
#endif

	free(spm, M_DMAMAP);
}

/*
 * Utility function used by splay tree to order page entries by pa.
 */
static inline int
iomap_compare(struct sg_page_entry *a, struct sg_page_entry *b)
{
	return ((a->spe_pa > b->spe_pa) ? 1 :
		(a->spe_pa < b->spe_pa) ? -1 : 0);
}

SPLAY_PROTOTYPE(sg_page_tree, sg_page_entry, spe_node, iomap_compare);

SPLAY_GENERATE(sg_page_tree, sg_page_entry, spe_node, iomap_compare);

/*
 * Insert a pa entry in the iomap.
 */
static int
sg_iomap_insert_page(struct sg_page_map *spm, paddr_t pa)
{
	struct sg_page_entry *e;

	if (spm->spm_pagecnt >= spm->spm_maxpage) {
		struct sg_page_entry spe;

		spe.spe_pa = pa;
		if (SPLAY_FIND(sg_page_tree, &spm->spm_tree, &spe))
			return (0);

#ifdef DIAGNOSTIC
		printf("%s: max page count reached: %d >= %d\n", __func__,
		    spm->spm_pagecnt, spm->spm_maxpage);
#endif
		return (ENOMEM);
	}

	e = &spm->spm_map[spm->spm_pagecnt];

	e->spe_pa = pa;
	e->spe_va = 0;

	e = SPLAY_INSERT(sg_page_tree, &spm->spm_tree, e);

	/* Duplicates are okay, but only count them once. */
	if (e)
		return (0);

	++spm->spm_pagecnt;

	return (0);
}

/*
 * Locate the iomap by filling in the pa->va mapping and inserting it
 * into the IOMMU tables.
 */
static void
sg_iomap_load_map(struct sg_cookie *sg, struct sg_page_map *spm,
    bus_addr_t vmaddr, int flags)
{
	struct sg_page_entry	*e;
	int			 i;

	for (i = 0, e = spm->spm_map; i < spm->spm_pagecnt; ++i, ++e) {
		e->spe_va = vmaddr;
		sg->bind_page(sg->sg_hdl, e->spe_va, e->spe_pa, flags);
		vmaddr += PAGE_SIZE;
	}
	sg->flush_tlb(sg->sg_hdl);
}

/*
 * Remove the iomap from the IOMMU.
 */
static void
sg_iomap_unload_map(struct sg_cookie *sg, struct sg_page_map *spm)
{
	struct sg_page_entry	*e;
	int			 i;

	for (i = 0, e = spm->spm_map; i < spm->spm_pagecnt; ++i, ++e)
		sg->unbind_page(sg->sg_hdl, e->spe_va);
	sg->flush_tlb(sg->sg_hdl);
}

/*
 * Translate a physical address (pa) into a DVMA address.
 */
static bus_addr_t
sg_iomap_translate(struct sg_page_map *spm, paddr_t pa)
{
	struct sg_page_entry	*e, pe;
	paddr_t			 offset = pa & PAGE_MASK;

	pe.spe_pa = trunc_page(pa);

	e = SPLAY_FIND(sg_page_tree, &spm->spm_tree, &pe);

	if (e == NULL) {
#ifdef DIAGNOSTIC
		printf("%s: phys addr %#"PRIxPADDR" not found\n", __func__, pa);
#endif
		return (0);
	}

	return (e->spe_va | offset);
}

/*
 * Clear the iomap table and tree.
 */
static void
sg_iomap_clear_pages(struct sg_page_map *spm)
{
	spm->spm_pagecnt = 0;
	SPLAY_INIT(&spm->spm_tree);
}
