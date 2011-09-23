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

#include <machine/cpu.h>

#include <x86/sg_dma.h>
#include <x86/bus_private.h>

#ifndef MAX_DMA_SEGS
#define MAX_DMA_SEGS	20
#endif

/* Disable 24-bit DMA support if it's not supported by arch. */
#ifndef BUS_DMA_24BIT
#define BUS_DMA_24BIT	0
#endif

static struct sg_page_map *sg_dmamap_to_spm(struct sg_cookie *, bus_dmamap_t);
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
static int	sg_dmamem_alloc(void *, bus_dma_tag_t, bus_size_t, bus_size_t,
		    bus_size_t, bus_dma_segment_t *, int, int *, int);

static struct sg_page_map *sg_iomap_create(int);
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
	static const struct bus_dma_overrides ov0 = {
		.ov_dmamap_create = sg_dmamap_create,
		.ov_dmamap_destroy = sg_dmamap_destroy,
		.ov_dmamap_load = sg_dmamap_load,
		.ov_dmamap_load_mbuf = sg_dmamap_load_mbuf,
		.ov_dmamap_load_uio = sg_dmamap_load_uio,
		.ov_dmamap_load_raw = sg_dmamap_load_raw,
		.ov_dmamap_unload = sg_dmamap_unload,
		.ov_dmamem_alloc = sg_dmamem_alloc
	};
	uint64_t present =
	    BUS_DMAMAP_OVERRIDE_CREATE |
	    BUS_DMAMAP_OVERRIDE_DESTROY |
	    BUS_DMAMAP_OVERRIDE_LOAD |
	    BUS_DMAMAP_OVERRIDE_LOAD_MBUF |
	    BUS_DMAMAP_OVERRIDE_LOAD_UIO |
	    BUS_DMAMAP_OVERRIDE_LOAD_RAW |
	    BUS_DMAMAP_OVERRIDE_UNLOAD |
	    BUS_DMAMEM_OVERRIDE_ALLOC
	    /* XXX: BUS_DMAMEM_OVERRIDE_FREE */;
	struct sg_cookie	*sg;
	int error;

	sg = kmem_zalloc(sizeof(*sg), KM_NOSLEEP);
	if (sg == NULL)
		return ENOMEM;

	sg->sg_ov = ov0;

	if (dmasync != NULL) {
		present |= BUS_DMAMAP_OVERRIDE_SYNC;
		sg->sg_ov.ov_dmamap_sync = dmasync;
	}

	sg->sg_ex = extent_create(name, start, start + size - 1,
	    M_DMAMAP, NULL, 0, EX_NOWAIT | EX_NOCOALESCE);
	if (sg->sg_ex == NULL) {
		error = ENOMEM;
		goto out;
	}

	sg->sg_hdl = hdl;
	sg->bind_page = bind;
	sg->unbind_page = unbind;
	sg->flush_tlb = flushtlb;
	sg->sg_dmat = odmat;

	error = bus_dma_tag_create(sg->sg_dmat, present, &sg->sg_ov, sg, dmat);
	if (error != 0)
		goto out;

	mutex_init(&sg->sg_mtx, MUTEX_DEFAULT, IPL_HIGH);

	return 0;
out:
	kmem_free(sg, sizeof(*sg));
	return error;
}

void
sg_dmatag_destroy(struct sg_cookie *sg)
{
	mutex_destroy(&sg->sg_mtx);
	extent_destroy(sg->sg_ex);
	kmem_free(sg, sizeof(*sg));
}

static struct sg_page_map *
sg_dmamap_to_spm(struct sg_cookie *sg, bus_dmamap_t map)
{
	int i;

	for (i = 0; i < sg->sg_mm_next; i++) {
		if (sg->sg_mm[i].mm_dmamap == map)
			return sg->sg_mm[i].mm_spm;
	}
	return NULL;
}

static int
sg_dmamap_create(void *cookie, bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *mapp)
{
	struct sg_page_map	*spm;
	int			 ret;
	struct sg_cookie	*sg;
	int i;

	sg = cookie;

	/* XXX synchronization? */
	if (sg->sg_mm_next >= __arraycount(sg->sg_mm))
		return ENOMEM;

	i = sg->sg_mm_next++;

	if ((ret = bus_dmamap_create(sg->sg_dmat, size, nsegments, maxsegsz,
	    boundary, flags, mapp)) != 0) {
		sg->sg_mm_next--;
		return ret;
	}

	if ((spm = sg_iomap_create(atop(round_page(size)))) == NULL) {
		bus_dmamap_destroy(sg->sg_dmat, *mapp);
		sg->sg_mm_next--;
		return ENOMEM;
	}

	sg->sg_mm[i].mm_dmamap = *mapp;
	sg->sg_mm[i].mm_spm = spm;

	return 0;
}

/* XXX WTF? --dyoung */
void
sg_dmamap_set_alignment(void *cookie, bus_dma_tag_t tag, bus_dmamap_t dmam,
    u_long alignment)
{
	struct sg_cookie *sg;
	if (alignment < PAGE_SIZE)
		return;
	sg = cookie;

	sg->sg_align = alignment;
}

static void
sg_dmamap_destroy(void *cookie, bus_dma_tag_t t, bus_dmamap_t map)
{
	struct sg_page_map *spm;
	struct sg_cookie *sg = cookie;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	/*
	 * The specification (man page) requires a loaded
	 * map to be unloaded before it is destroyed.
	 */
	if (map->dm_nsegs != 0)
		bus_dmamap_unload(t, map);

	sg_iomap_destroy(spm);

	/* XXX reclaim sg_mm[] slot */

	bus_dmamap_destroy(sg->sg_dmat, map);
}

static int
sg_map_segments(struct sg_cookie *sg, struct sg_page_map *spm, bus_dmamap_t map,
    int flags)
{
	bus_dma_segment_t *ds;
	int err, i;
	bus_size_t sgsize;
	u_long dvmaddr;
	bus_size_t align, boundary;

	/*
	 * A boundary presented to bus_dmamem_alloc() takes precedence
	 * over boundary in the map.
	 */
	if ((boundary = (sg->sg_boundary)) == 0)
		boundary = map->_dm_boundary;
	align = MAX(sg->sg_align, PAGE_SIZE);

	/* Count up the total number of pages we need */
	sg_iomap_clear_pages(spm);
	for (i = 0; i < map->dm_nsegs; i++) {
		paddr_t pa;

		ds = &map->dm_segs[i];

		for (pa = trunc_page(ds->ds_addr);
		     pa <= trunc_page(ds->ds_addr + ds->ds_len - 1);
		     pa += PAGE_SIZE) {
			err = sg_iomap_insert_page(spm, pa);
			if (err) {
				printf("iomap insert error: %d for "
				    "pa %#" PRIxPADDR "\n",
				    err, pa);
				sg_iomap_clear_pages(spm);
				return EFBIG;
			}
			spm->spm_oaddrs[i] = ds->ds_addr;
			ds->ds_addr = sg_iomap_translate(spm, ds->ds_addr);
		}
	}

	sgsize = spm->spm_pagecnt * PAGE_SIZE;

	mutex_enter(&sg->sg_mtx);

	/*
	 * If our segment size sg larger than the boundary we need to
	 * split the transfer up into little pieces ourselves.
	 */
	err = extent_alloc_subregion(sg->sg_ex,
	    sg->sg_ex->ex_start, sg->sg_ex->ex_end,
	    sgsize, align, (sgsize > boundary) ? 0 : boundary,
	    EX_NOWAIT | EX_BOUNDZERO, (u_long *)&dvmaddr);
	mutex_exit(&sg->sg_mtx);
	if (err != 0) {
		sg_iomap_clear_pages(spm);
		return err;
	}

	/* Set the active DVMA map */
	spm->spm_start = dvmaddr;
	spm->spm_size = sgsize;

	sg_iomap_load_map(sg, spm, dvmaddr, flags);

	return 0;
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
sg_dmamap_load(void *cookie, bus_dma_tag_t t, bus_dmamap_t map, void *buf,
    bus_size_t buflen, struct proc *p, int flags)
{
	int err;
	struct sg_cookie *sg = cookie;
	struct sg_page_map *spm;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	if (map->dm_nsegs != 0)
		panic("%s: map still in use", __func__);

	err = bus_dmamap_load(sg->sg_dmat, map, buf, buflen, p, flags);
	if (err != 0) {
		/*
		 * Make sure that on error condition we return
		 * "no valid mappings".
		 */
		map->dm_nsegs = 0;
		return err;
	}

	if ((err = sg_map_segments(sg, spm, map, flags)) != 0) {
		bus_dmamap_unload(sg->sg_dmat, map);
		map->dm_nsegs = 0;
	} else {
		spm->spm_origbuf = buf;
		spm->spm_buftype = X86_DMA_BUFTYPE_LINEAR;
		spm->spm_proc = p;
	}
	return err;
}

/*
 * Load an mbuf into our map. we convert it to some bus_dma_segment_ts then
 * pass it to load_raw.
 */
static int
sg_dmamap_load_mbuf(void *cookie, bus_dma_tag_t t, bus_dmamap_t map,
    struct mbuf *mb, int flags)
{
	struct sg_page_map	*spm;
	struct sg_cookie	*sg = cookie;
	int			 err;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	err = bus_dmamap_load_mbuf(sg->sg_dmat, map, mb, flags);
	if (err != 0) {
		/*
		 * Make sure that on error condition we return
		 * "no valid mappings".
		 */
		map->dm_nsegs = 0;
		return err;
	}

	if ((err = sg_map_segments(sg, spm, map, flags)) != 0) {
		bus_dmamap_unload(sg->sg_dmat, map);
		map->dm_nsegs = 0;
	} else {
		spm->spm_origbuf = mb;
		spm->spm_buftype = X86_DMA_BUFTYPE_MBUF;
	}
	return err;
}

/*
 * Load a uio into the map. Turn it into segments and call load_raw()
 *
 * Loading uios is kinda broken since we can't lock the pages
 * and unlock them at unload.  Perhaps page loaning is the answer.
 * Until then we only accept kernel data.
 */
static int
sg_dmamap_load_uio(void *cookie, bus_dma_tag_t t, bus_dmamap_t map,
    struct uio *uio, int flags)
{
	struct sg_page_map	*spm;
	struct sg_cookie	*sg = cookie;
	int			 err;

	if (!VMSPACE_IS_KERNEL_P(uio->uio_vmspace))
		return EOPNOTSUPP;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	err = bus_dmamap_load_uio(sg->sg_dmat, map, uio, flags);
	if (err != 0) {
		/*
		 * Make sure that on error condition we return
		 * "no valid mappings".
		 */
		map->dm_nsegs = 0;
		return err;
	}

	if ((err = sg_map_segments(sg, spm, map, flags)) != 0) {
		bus_dmamap_unload(sg->sg_dmat, map);
		map->dm_nsegs = 0;
	} else {
		spm->spm_origbuf = uio;
		spm->spm_buftype = X86_DMA_BUFTYPE_UIO;
	}
	return err;
}

/*
 * Load a dvmamap from an array of segs.  It calls sg_dmamap_append_range()
 * or for part of the 2nd pass through the mapping.
 */
static int
sg_dmamap_load_raw(void *cookie, bus_dma_tag_t t, bus_dmamap_t map,
    bus_dma_segment_t *segs, int nsegs, bus_size_t size, int flags)
{
	int err;
	struct sg_cookie *sg = cookie;
	struct sg_page_map *spm;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	err = bus_dmamap_load_raw(sg->sg_dmat, map, segs, nsegs, size, flags);
	if (err != 0) {
		/*
		 * Make sure that on error condition we return
		 * "no valid mappings".
		 */
		map->dm_nsegs = 0;
		return err;
	}

	if ((err = sg_map_segments(sg, spm, map, flags)) != 0) {
		bus_dmamap_unload(sg->sg_dmat, map);
		map->dm_nsegs = 0;
	} else {
		spm->spm_origbuf = segs;
		spm->spm_buftype = X86_DMA_BUFTYPE_RAW;
	}

	return err;
}

/*
 * Unload a dvmamap.
 */
static void
sg_dmamap_unload(void *cookie, bus_dma_tag_t t, bus_dmamap_t map)
{
	struct sg_cookie	*sg = cookie;
	struct sg_page_map	*spm;
	bus_addr_t		 dvmaddr;
	bus_size_t		 sgsize;
	int			 error, i;

	if ((spm = sg_dmamap_to_spm(sg, map)) == NULL)
		panic("%s: unknown dmamap", __func__);

	dvmaddr = spm->spm_start;
	sgsize = spm->spm_size;

	/* Remove the IOMMU entries */
	sg_iomap_unload_map(sg, spm);

	/* Clear the iomap */
	sg_iomap_clear_pages(spm);

	mutex_enter(&sg->sg_mtx);
	error = extent_free(sg->sg_ex, dvmaddr, sgsize, EX_NOWAIT);
	spm->spm_start = 0;
	spm->spm_size = 0;
	mutex_exit(&sg->sg_mtx);
	if (error != 0)
		printf("warning: %zd of DVMA space lost\n", sgsize);

	spm->spm_buftype = X86_DMA_BUFTYPE_INVALID;
	spm->spm_origbuf = NULL;
	spm->spm_proc = NULL;

	for (i = 0; i < map->dm_nsegs; i++)
		map->dm_segs[i].ds_addr = spm->spm_oaddrs[i];

	bus_dmamap_unload(sg->sg_dmat, map);
}

/*
 * Alloc dma safe memory, telling the backend that we're scatter gather
 * to ease pressure on the vm.
 *
 * This assumes that we can map all physical memory.
 */
static int
sg_dmamem_alloc(void *cookie, bus_dma_tag_t t, bus_size_t size,
    bus_size_t alignment, bus_size_t boundary, bus_dma_segment_t *segs,
    int nsegs, int *rsegs, int flags)
{
	struct sg_cookie *sg = cookie;
	sg->sg_align = alignment;
	sg->sg_boundary = boundary;
	return bus_dmamem_alloc(sg->sg_dmat, size, 0, 0, segs, nsegs, rsegs,
	    flags);
}

static size_t
spm_size(int n)
{
	return offsetof(struct sg_page_map, spm_map[n]);
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

	spm = kmem_zalloc(spm_size(n), KM_NOSLEEP);
	if (spm == NULL)
		return NULL;

	spm->spm_oaddrs = kmem_zalloc(n * sizeof(*spm->spm_oaddrs), KM_NOSLEEP);

	if (spm->spm_oaddrs == NULL) {
		kmem_free(spm, spm_size(n));
		return NULL;
	}

	/* Initialize the map. */
	spm->spm_maxpage = n;
	SPLAY_INIT(&spm->spm_tree);

	return spm;
}

/*
 * Destroy an iomap.
 */
static void
sg_iomap_destroy(struct sg_page_map *spm)
{
	KASSERT(spm->spm_pagecnt == 0);

	kmem_free(spm->spm_oaddrs, spm->spm_maxpage * sizeof(*spm->spm_oaddrs));
	kmem_free(spm, spm_size(spm->spm_maxpage));
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

	if (e == NULL)
		return (0);

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
