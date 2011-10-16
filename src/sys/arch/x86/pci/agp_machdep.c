/*	$OpenBSD: agp_machdep.c,v 1.6 2010/05/10 22:06:04 oga Exp $	*/

/*
 * Copyright (c) 2008 - 2009 Owain G. Ainsworth <oga@openbsd.org>
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
 * Copyright (c) 2002 Michael Shalayeff
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/device.h>
#include <sys/bus.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/agpvar.h>
#include <dev/pci/agpreg.h>

#include <x86/sg_dma.h>

#include <machine/bus_private.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>

#include "agp_i810.h"

/* bus_dma functions */

#if NAGP_I810 > 0
void	intagp_dma_sync(void *, bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
	    bus_size_t, int);
#endif /* NAGP_I810 > 0 */

static void	agp_sg_bind_page(void *, bus_addr_t, paddr_t, int);
static void	agp_sg_unbind_page(void *, bus_addr_t);
static void	agp_sg_flush_tlb(void *);

void
agp_flush_cache(void)
{
	wbinvd();
}

void
agp_flush_cache_range(vaddr_t va, vsize_t sz)
{
#if defined(__HAVE_PMAP_FLUSH_CACHE)
	pmap_flush_cache(va, sz);
#else /* defined(__HAVE_PMAP_FLUSH_CACHE) */
	wbinvd();
#endif /* defined(__HAVE_PMAP_FLUSH_CACHE) */
}

/*
 * functions for bus_dma used by drm for GEM
 *
 * We use the sg_dma backend (also used by iommu) to provide the actual
 * implementation, so all we need provide is the magic to create the tag, and
 * the appropriate callbacks.
 *
 * We give the backend drivers a chance to honour the bus_dma flags, some of
 * these may be used, for example to provide snooped mappings (intagp).
 * For intagp at least, we honour the BUS_DMA_COHERENT flag, though it is not
 * used often, and is * technically to be used for dmamem_map, we use it for
 * dmamap_load since adding coherency involes flags to the gtt pagetables.
 * We only use it for very special circumstances since when a GTT mapping is
 * set to coherent, the cpu can't read or write through the gtt aperture.
 *
 * Currently, since the userland agp driver still needs to access the gart, we
 * only do bus_dma for a section that we've been told is ours, hence the need
 * for the init function at present.
 */

static void
agp_sg_bind_page(void *dev, bus_addr_t address, paddr_t physical, int flags)
{
	struct agp_softc *sc = dev;
	int error;

	error = AGP_BIND_PAGE(sc, address - sc->as_apaddr,
	    _BUS_PHYS_TO_BUS(physical), flags);
	if (error)
		aprint_error_dev(sc->as_dev,
		    "%s: failed: ba %#"PRIxPADDR", pa %#"PRIxPADDR"\n",
		    __func__, address, physical);
}

static void
agp_sg_unbind_page(void *dev, bus_addr_t address)
{
	struct agp_softc *sc = dev;
	int error;

	error = AGP_UNBIND_PAGE(sc, address - sc->as_apaddr);
	if (error)
		aprint_error_dev(sc->as_dev, "%s: failed: ba %#"PRIxPADDR"\n",
		    __func__, address);
}

static void
agp_sg_flush_tlb(void *dev)
{
	struct agp_softc *sc = dev;

	AGP_FLUSH_TLB(sc);
}

int
agp_bus_dma_init(struct agp_softc *sc, bus_dma_tag_t odmat, bus_addr_t start,
    bus_addr_t end, bus_dma_tag_t *dmat)
{

	/*
	 * XXX add agp map into the main queue that takes up our chunk of
	 * GTT space to prevent the userland api stealing any of it.
	 */
	return sg_dmatag_create("agpgtt", sc, odmat, start, end - start,
	    agp_sg_bind_page, agp_sg_unbind_page, agp_sg_flush_tlb,
	    sc->as_methods->dma_sync, dmat);
}

void
agp_bus_dma_destroy(struct agp_softc *sc, bus_dma_tag_t dmat)
{
	/* XXX We probably shouldn't directly access bdt_ctx. */
	struct sg_cookie	*cookie = dmat->bdt_ctx;
	bus_addr_t		 offset;


	/*
	 * XXX clear up blocker queue
	 */

	/*
	 * some backends use a dummy page to avoid errors on prefetching, etc.
	 * make sure that all of them are clean.
	 */
	for (offset = cookie->sg_ex->ex_start;
	    offset < cookie->sg_ex->ex_end; offset += PAGE_SIZE)
		agp_sg_unbind_page(sc, offset);

	sg_dmatag_destroy(cookie);
	bus_dma_tag_destroy(dmat);
}

void
agp_bus_dma_set_alignment(bus_dma_tag_t tag, bus_dmamap_t dmam,
    u_long alignment)
{
	sg_dmamap_set_alignment(tag, dmam, alignment);
}

struct agp_map {
	bus_space_tag_t		bst;
	bus_size_t		size;
#ifdef __i386__
	bus_addr_t		addr;
	int			flags;
#else
	bus_space_handle_t	bsh;
#endif
};

#ifdef __i386__
extern struct extent	*ioport_ex;
extern struct extent	*iomem_ex;
#endif

int
agp_init_map(bus_space_tag_t tag, bus_addr_t address, bus_size_t size,
    int flags, struct agp_map **mapp)
{
	struct agp_map	*map;
	int		 error;
#ifdef __i386__
	struct extent	*ex;

	if (tag->bst_type == X86_BUS_SPACE_IO) {
		ex = ioport_ex;
		if (flags & BUS_SPACE_MAP_LINEAR)
			return (EINVAL);
	} else if (tag->bst_type == X86_BUS_SPACE_MEM) {
		ex = iomem_ex;
	} else {
		panic("agp_init_map: bad bus space tag");
	}
	/*
	 * We grab the extent out of the bus region ourselves
	 * so we don't need to do these allocations every time.
	 */
	error = extent_alloc_region(ex, address, size,
	    EX_NOWAIT | EX_MALLOCOK);
	if (error)
		return (error);
#endif

	map = malloc(sizeof(*map), M_AGP, M_WAITOK | M_CANFAIL);
	if (map == NULL)
		return (ENOMEM);

	map->bst = tag;
	map->size = size;
#ifdef __i386__
	map->addr = address;
	map->flags = flags;
#else
	if ((error = bus_space_map(tag, address, size, flags, &map->bsh)) != 0) {
		free(map, M_AGP);
		return (error);
	}
#endif

	*mapp = map;
	return (0);
}

void
agp_destroy_map(struct agp_map *map)
{
#ifdef __i386__
	struct extent	*ex;

	if (map->bst->bst_type == X86_BUS_SPACE_IO)
		ex = ioport_ex;
	else if (map->bst->bst_type == X86_BUS_SPACE_MEM)
		ex = iomem_ex;
	else
		panic("agp_destroy_map: bad bus space tag");

	if (extent_free(ex, map->addr, map->size,
	    EX_NOWAIT | EX_MALLOCOK ))
		printf("agp_destroy_map: can't free region\n");
#else
	bus_space_unmap(map->bst, map->bsh, map->size);
#endif
	free(map, M_AGP);
}


int
agp_map_subregion(struct agp_map *map, bus_size_t offset, bus_size_t size,
    bus_space_handle_t *bshp)
{
#ifdef __i386__
	return (_x86_memio_map(map->bst, map->addr + offset, size,
	    map->flags, bshp));
#else
	if (offset > map->size || size > map->size || offset + size > map->size)
		return (EINVAL);
	return (bus_space_subregion(map->bst, map->bsh, offset, size, bshp));
#endif
}

void
agp_unmap_subregion(struct agp_map *map, bus_space_handle_t bsh,
    bus_size_t size)
{
#ifdef __i386__
	return (_x86_memio_unmap(map->bst, bsh, size, NULL));
#else
	/* subregion doesn't need unmapping, do nothing */
#endif
}

/*
 * ick ick ick. However, the rest of this driver is supposedly MI (though
 * they only exist on x86), so this can't be in dev/pci.
 */

#if NAGP_I810 > 0

/*
 * bus_dmamap_sync routine for intagp.
 *
 * This is tailored to the usage that drm with the GEM memory manager
 * will be using, since intagp is for intel IGD, and thus shouldn't be
 * used for anything other than gpu-based work. Essentially for the intel GEM
 * driver we use bus_dma as an abstraction to convert our memory into a gtt
 * address and deal with any cache incoherencies that we create.
 *
 * We use the cflush instruction to deal with clearing the caches, since our
 * cache is physically indexed, we can even map then clear the page and it'll
 * work. on i386 we need to check for the presence of cflush() in cpuid,
 * however, all cpus that have a new enough intel GMCH should be suitable.
 */
void
intagp_dma_sync(void *ctx, bus_dma_tag_t tag, bus_dmamap_t dmam,
    bus_addr_t offset, bus_size_t size, int ops)
{
#if defined(__HAVE_PMAP_FLUSH_CACHE) && defined(__HAVE_PMAP_FLUSH_PAGE)
	bus_dma_segment_t	*segp;
	struct sg_page_map	*spm;
	vaddr_t			 addr;
	paddr_t	 		 pa;
	bus_addr_t		 poff, endoff, soff;
#endif /* defined(__HAVE_PMAP_FLUSH_CACHE) && defined(__HAVE_PMAP_FLUSH_PAGE) */

#ifdef DIAGNOSTIC
	if ((ops & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE)) != 0 &&
	    (ops & (BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE)) != 0)
		panic("agp_dmamap_sync: mix PRE and POST");
	if (offset >= dmam->dm_mapsize)
		panic("_intagp_dma_sync: bad offset %"PRIxPADDR" (size = %zu)",
		    offset, dmam->dm_mapsize);
	if (size == 0 || (offset + size) > dmam->dm_mapsize)
		panic("intagp_dma_sync: bad length");
#endif /* DIAGNOSTIC */

	/* Coherent mappings need no sync. */
	if (dmam->_dm_flags & BUS_DMA_COHERENT)
		return;

	/*
	 * We need to clflush the object cache in all cases but postwrite.
	 *
	 * - Due to gpu incoherency, postread we need to flush speculative
	 * reads (which are not written back on intel cpus).
	 *
	 * - preread we need to flush data which will very soon be stale from
	 * the caches
	 *
	 * - prewrite we need to make sure our data hits the memory before the
	 * gpu hoovers it up.
	 *
	 * The chipset also may need flushing, but that fits badly into
	 * bus_dma and it done in the driver.
	 */
	if (ops & BUS_DMASYNC_POSTREAD || ops & BUS_DMASYNC_PREREAD ||
	    ops & BUS_DMASYNC_PREWRITE) {
#if defined(__HAVE_PMAP_FLUSH_CACHE) && defined(__HAVE_PMAP_FLUSH_PAGE)
		if (curcpu()->ci_cflush_lsize == 0) {
			/* save some wbinvd()s. we're MD anyway so it's ok */
			wbinvd();
			return;
		}

		soff = trunc_page(offset);
		endoff = round_page(offset + size);
		x86_mfence();
		spm = dmam->_dm_sg_cookie;
		switch (spm->spm_buftype) {
		case X86_DMA_BUFTYPE_LINEAR:
			addr = (vaddr_t)spm->spm_origbuf + soff;
			while (soff < endoff) {
				pmap_flush_cache(addr, PAGE_SIZE);
				soff += PAGE_SIZE;
				addr += PAGE_SIZE;
			} break;
		case X86_DMA_BUFTYPE_RAW:
			segp = (bus_dma_segment_t *)spm->spm_origbuf;
			poff = 0;

			while (poff < soff) {
				if (poff + segp->ds_len > soff)
					break;
				poff += segp->ds_len;
				segp++;
			}
			/* first time round may not start at seg beginning */
			pa = segp->ds_addr + (soff - poff);
			while (poff < endoff) {
				for (; pa < segp->ds_addr + segp->ds_len &&
				    poff < endoff; pa += PAGE_SIZE) {
					pmap_flush_page(pa);
					poff += PAGE_SIZE;
				}
				segp++;
				if (poff < endoff)
					pa = segp->ds_addr;
			}
			break;
		/* You do not want to load mbufs or uios onto a graphics card */
		case X86_DMA_BUFTYPE_MBUF:
			/* FALLTHROUGH */
		case X86_DMA_BUFTYPE_UIO:
			/* FALLTHROUGH */
		default:
			panic("intagp_dmamap_sync: bad buftype %d",
			    spm->spm_buftype);
		}
		x86_mfence();
#else /* defined(__HAVE_PMAP_FLUSH_CACHE) && defined(__HAVE_PMAP_FLUSH_PAGE) */
		wbinvd();
#endif /* defined(__HAVE_PMAP_FLUSH_CACHE) && defined(__HAVE_PMAP_FLUSH_PAGE) */
	}
}
#endif /* NAGP_I810 > 0 */
