/*	$NetBSD: agp_i810.c,v 1.74 2014/03/18 18:20:41 riastradh Exp $	*/

/*-
 * Copyright (c) 2000 Doug Rabson
 * Copyright (c) 2000 Ruslan Ermilov
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD$
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: agp_i810.c,v 1.74 2014/03/18 18:20:41 riastradh Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/device.h>
#include <sys/conf.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>
#include <dev/pci/agpvar.h>
#include <dev/pci/agpreg.h>
#include <dev/pci/agp_i810var.h>

#include <sys/agpio.h>

#include <sys/bus.h>

#include "agp_intel.h"

struct agp_softc *agp_i810_sc = NULL;

#define READ1(off)	bus_space_read_1(isc->bst, isc->bsh, off)
#define READ4(off)	bus_space_read_4(isc->bst, isc->bsh, off)
#define WRITE4(off,v)	bus_space_write_4(isc->bst, isc->bsh, off, v)

/* Memory is snooped, must not be accessed through gtt from the cpu. */
#define	INTEL_COHERENT	0x6

#define CHIP_I810 0	/* i810/i815 */
#define CHIP_I830 1	/* 830M/845G */
#define CHIP_I855 2	/* 852GM/855GM/865G */
#define CHIP_I915 3	/* 915G/915GM/945G/945GM/945GME */
#define CHIP_I965 4	/* 965Q/965PM */
#define CHIP_G33  5	/* G33/Q33/Q35 */
#define CHIP_G4X  6	/* G45/Q45 */
#define CHIP_SNB  7	/* Sandy/Ivy Bridge */

/* XXX hack, see below */
static bool agp_i810_vga_mapped = false;
static bus_addr_t agp_i810_vga_regbase;
static bus_space_tag_t agp_i810_vga_bst;
static bus_space_handle_t agp_i810_vga_bsh;

static u_int32_t agp_i810_get_aperture(struct agp_softc *);
static int agp_i810_set_aperture(struct agp_softc *, u_int32_t);
static int agp_i810_bind_page(struct agp_softc *, off_t, bus_addr_t, int);
static int agp_i810_unbind_page(struct agp_softc *, off_t);
static void agp_i810_flush_tlb(struct agp_softc *);
static int agp_i810_enable(struct agp_softc *, u_int32_t mode);
static struct agp_memory *agp_i810_alloc_memory(struct agp_softc *, int,
						vsize_t);
static int agp_i810_free_memory(struct agp_softc *, struct agp_memory *);
static int agp_i810_bind_memory(struct agp_softc *, struct agp_memory *, off_t);
static int agp_i810_unbind_memory(struct agp_softc *, struct agp_memory *);

static bool agp_i810_resume(device_t, const pmf_qual_t *);
static int agp_i810_init(struct agp_softc *);

static int agp_i810_init(struct agp_softc *);

extern void intagp_dma_sync(void *, bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
			    bus_size_t, int);

static struct agp_methods agp_i810_methods = {
	.get_aperture	= agp_i810_get_aperture,
	.set_aperture	= agp_i810_set_aperture,
	.bind_page	= agp_i810_bind_page,
	.unbind_page	= agp_i810_unbind_page,
	.flush_tlb	= agp_i810_flush_tlb,
	.dma_sync	= intagp_dma_sync,
	.enable		= agp_i810_enable,
	.alloc_memory	= agp_i810_alloc_memory,
	.free_memory	= agp_i810_free_memory,
	.bind_memory	= agp_i810_bind_memory,
	.unbind_memory	= agp_i810_unbind_memory,
};

int
agp_i810_write_gtt_entry(struct agp_i810_softc *isc, off_t off, bus_addr_t v)
{
	u_int32_t pte;
	bus_size_t base_off, wroff;

	/* Bits 11:4 (physical start address extension) should be zero. */
	if ((v & 0xff0) != 0)
		return EINVAL;

	pte = (u_int32_t)v;
	/*
	 * We need to massage the pte if bus_addr_t is wider than 32 bits.
	 * The compiler isn't smart enough, hence the casts to uintmax_t.
	 */
	if (sizeof(bus_addr_t) > sizeof(u_int32_t)) {
		/* gen6+ can do 40 bit addressing. */
		if (isc->chiptype == CHIP_SNB) {
			if (((uintmax_t)v >> 40) != 0)
				return EINVAL;
			pte |= (v >> 28) & 0xff0;
		/* 965+ can do 36-bit addressing, add in the extra bits. */
		} else if (isc->chiptype == CHIP_I965 ||
		    isc->chiptype == CHIP_G33 ||
		    isc->chiptype == CHIP_G4X) {
			if (((uintmax_t)v >> 36) != 0)
				return EINVAL;
			pte |= (v >> 28) & 0xf0;
		} else {
			if (((uintmax_t)v >> 32) != 0)
				return EINVAL;
		}
	}

	base_off = 0;
	wroff = (off >> AGP_PAGE_SHIFT) * 4;

	switch (isc->chiptype) {
	case CHIP_I810:
	case CHIP_I830:
	case CHIP_I855:
		base_off = AGP_I810_GTT;
		break;
	case CHIP_I965:
		base_off = AGP_I965_GTT;
		break;
	case CHIP_G4X:
	case CHIP_SNB:
		base_off = AGP_G4X_GTT;
		break;
	case CHIP_I915:
	case CHIP_G33:
		bus_space_write_4(isc->gtt_bst, isc->gtt_bsh, wroff, pte);
		return 0;
	}

	WRITE4(base_off + wroff, pte);
	return 0;
}

void
agp_i810_post_gtt_entry(struct agp_i810_softc *isc, off_t off)
{
	bus_size_t base_off, wroff;

	base_off = 0;
	wroff = (off >> AGP_PAGE_SHIFT) * 4;

	switch (isc->chiptype) {
	case CHIP_I810:
	case CHIP_I830:
	case CHIP_I855:
		base_off = AGP_I810_GTT;
		break;
	case CHIP_I965:
		base_off = AGP_I965_GTT;
		break;
	case CHIP_G4X:
		base_off = AGP_G4X_GTT;
		break;
	case CHIP_I915:
	case CHIP_G33:
		(void)bus_space_read_4(isc->gtt_bst, isc->gtt_bsh, wroff);
		return;
	}

	(void)READ4(base_off + wroff);
}

/* XXXthorpej -- duplicated code (see arch/x86/pci/pchb.c) */
static int
agp_i810_vgamatch(const struct pci_attach_args *pa)
{

	if (PCI_CLASS(pa->pa_class) != PCI_CLASS_DISPLAY ||
	    PCI_SUBCLASS(pa->pa_class) != PCI_SUBCLASS_DISPLAY_VGA)
		return (0);

	switch (PCI_PRODUCT(pa->pa_id)) {
	case PCI_PRODUCT_INTEL_82810_GC:
	case PCI_PRODUCT_INTEL_82810_DC100_GC:
	case PCI_PRODUCT_INTEL_82810E_GC:
	case PCI_PRODUCT_INTEL_82815_FULL_GRAPH:
	case PCI_PRODUCT_INTEL_82830MP_IV:
	case PCI_PRODUCT_INTEL_82845G_IGD:
	case PCI_PRODUCT_INTEL_82855GM_IGD:
	case PCI_PRODUCT_INTEL_82865_IGD:
	case PCI_PRODUCT_INTEL_82915G_IGD:
	case PCI_PRODUCT_INTEL_82915GM_IGD:
	case PCI_PRODUCT_INTEL_82945P_IGD:
	case PCI_PRODUCT_INTEL_82945GM_IGD:
	case PCI_PRODUCT_INTEL_82945GM_IGD_1:
	case PCI_PRODUCT_INTEL_82945GME_IGD:
	case PCI_PRODUCT_INTEL_E7221_IGD:
	case PCI_PRODUCT_INTEL_82965Q_IGD:
	case PCI_PRODUCT_INTEL_82965Q_IGD_1:
	case PCI_PRODUCT_INTEL_82965PM_IGD:
	case PCI_PRODUCT_INTEL_82965PM_IGD_1:
	case PCI_PRODUCT_INTEL_82G33_IGD:
	case PCI_PRODUCT_INTEL_82G33_IGD_1:
	case PCI_PRODUCT_INTEL_82965G_IGD:
	case PCI_PRODUCT_INTEL_82965G_IGD_1:
	case PCI_PRODUCT_INTEL_82965GME_IGD:
	case PCI_PRODUCT_INTEL_82Q35_IGD:
	case PCI_PRODUCT_INTEL_82Q35_IGD_1:
	case PCI_PRODUCT_INTEL_82Q33_IGD:
	case PCI_PRODUCT_INTEL_82Q33_IGD_1:
	case PCI_PRODUCT_INTEL_82G35_IGD:
	case PCI_PRODUCT_INTEL_82G35_IGD_1:
	case PCI_PRODUCT_INTEL_82946GZ_IGD:
	case PCI_PRODUCT_INTEL_82GM45_IGD:
	case PCI_PRODUCT_INTEL_82GM45_IGD_1:
	case PCI_PRODUCT_INTEL_82IGD_E_IGD:
	case PCI_PRODUCT_INTEL_82Q45_IGD:
	case PCI_PRODUCT_INTEL_82G45_IGD:
	case PCI_PRODUCT_INTEL_82G41_IGD:
	case PCI_PRODUCT_INTEL_82B43_IGD:
	case PCI_PRODUCT_INTEL_IRONLAKE_D_IGD:
	case PCI_PRODUCT_INTEL_IRONLAKE_M_IGD:
	case PCI_PRODUCT_INTEL_PINEVIEW_IGD:
	case PCI_PRODUCT_INTEL_PINEVIEW_M_IGD:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD_1:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD_2:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD_1:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD_2:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_IGD_1:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_M_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_M_IGD_1:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_S_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_S_IGD_1:
		return (1);
	}

	return (0);
}

static int
agp_i965_map_aperture(struct pci_attach_args *pa, struct agp_softc *sc, int reg)
{
        /*
         * Find the aperture. Don't map it (yet), this would
         * eat KVA.
         */
        if (pci_mapreg_info(pa->pa_pc, pa->pa_tag, reg,
            PCI_MAPREG_TYPE_MEM|PCI_MAPREG_MEM_TYPE_64BIT, &sc->as_apaddr, &sc->as_apsize,
            &sc->as_apflags) != 0)
                return ENXIO;

        sc->as_apt = pa->pa_memt;

        return 0;
}

int
agp_i810_attach(device_t parent, device_t self, void *aux)
{
	struct agp_softc *sc = device_private(self);
	struct agp_i810_softc *isc;
	struct agp_gatt *gatt;
	int error, apbase;
	bus_addr_t mmadr;
	bus_size_t mmadrsize;

	isc = malloc(sizeof *isc, M_AGP, M_NOWAIT|M_ZERO);
	if (isc == NULL) {
		aprint_error(": can't allocate chipset-specific softc\n");
		return ENOMEM;
	}
	sc->as_chipc = isc;
	sc->as_methods = &agp_i810_methods;

	if (pci_find_device(&isc->vga_pa, agp_i810_vgamatch) == 0) {
#if NAGP_INTEL > 0
		const struct pci_attach_args *pa = aux;

		switch (PCI_PRODUCT(pa->pa_id)) {
		case PCI_PRODUCT_INTEL_82840_HB:
		case PCI_PRODUCT_INTEL_82865_HB:
		case PCI_PRODUCT_INTEL_82845G_DRAM:
		case PCI_PRODUCT_INTEL_82815_FULL_HUB:
		case PCI_PRODUCT_INTEL_82855GM_MCH:
			return agp_intel_attach(parent, self, aux);
		}
#endif
		aprint_error(": can't find internal VGA device config space\n");
		free(isc, M_AGP);
		return ENOENT;
	}

	/* XXXfvdl */
	sc->as_dmat = isc->vga_pa.pa_dmat;

	switch (PCI_PRODUCT(isc->vga_pa.pa_id)) {
	case PCI_PRODUCT_INTEL_82810_GC:
	case PCI_PRODUCT_INTEL_82810_DC100_GC:
	case PCI_PRODUCT_INTEL_82810E_GC:
	case PCI_PRODUCT_INTEL_82815_FULL_GRAPH:
		isc->chiptype = CHIP_I810;
		break;
	case PCI_PRODUCT_INTEL_82830MP_IV:
	case PCI_PRODUCT_INTEL_82845G_IGD:
		isc->chiptype = CHIP_I830;
		break;
	case PCI_PRODUCT_INTEL_82855GM_IGD:
	case PCI_PRODUCT_INTEL_82865_IGD:
		isc->chiptype = CHIP_I855;
		break;
	case PCI_PRODUCT_INTEL_82915G_IGD:
	case PCI_PRODUCT_INTEL_82915GM_IGD:
	case PCI_PRODUCT_INTEL_82945P_IGD:
	case PCI_PRODUCT_INTEL_82945GM_IGD:
	case PCI_PRODUCT_INTEL_82945GM_IGD_1:
	case PCI_PRODUCT_INTEL_82945GME_IGD:
	case PCI_PRODUCT_INTEL_E7221_IGD:
	case PCI_PRODUCT_INTEL_PINEVIEW_IGD:
	case PCI_PRODUCT_INTEL_PINEVIEW_M_IGD:
		isc->chiptype = CHIP_I915;
		break;
	case PCI_PRODUCT_INTEL_82965Q_IGD:
	case PCI_PRODUCT_INTEL_82965Q_IGD_1:
	case PCI_PRODUCT_INTEL_82965PM_IGD:
	case PCI_PRODUCT_INTEL_82965PM_IGD_1:
	case PCI_PRODUCT_INTEL_82965G_IGD:
	case PCI_PRODUCT_INTEL_82965G_IGD_1:
	case PCI_PRODUCT_INTEL_82965GME_IGD:
	case PCI_PRODUCT_INTEL_82946GZ_IGD:
	case PCI_PRODUCT_INTEL_82G35_IGD:
	case PCI_PRODUCT_INTEL_82G35_IGD_1:
		isc->chiptype = CHIP_I965;
		break;
	case PCI_PRODUCT_INTEL_82Q35_IGD:
	case PCI_PRODUCT_INTEL_82Q35_IGD_1:
	case PCI_PRODUCT_INTEL_82G33_IGD:
	case PCI_PRODUCT_INTEL_82G33_IGD_1:
	case PCI_PRODUCT_INTEL_82Q33_IGD:
	case PCI_PRODUCT_INTEL_82Q33_IGD_1:
		isc->chiptype = CHIP_G33;
		break;
	case PCI_PRODUCT_INTEL_82GM45_IGD:
	case PCI_PRODUCT_INTEL_82GM45_IGD_1:
	case PCI_PRODUCT_INTEL_82IGD_E_IGD:
	case PCI_PRODUCT_INTEL_82Q45_IGD:
	case PCI_PRODUCT_INTEL_82G45_IGD:
	case PCI_PRODUCT_INTEL_82G41_IGD:
	case PCI_PRODUCT_INTEL_82B43_IGD:
	case PCI_PRODUCT_INTEL_IRONLAKE_D_IGD:
	case PCI_PRODUCT_INTEL_IRONLAKE_M_IGD:
		isc->chiptype = CHIP_G4X;
		break;
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD_1:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_IGD_2:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD_1:
	case PCI_PRODUCT_INTEL_SANDYBRIDGE_M_IGD_2:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_IGD_1:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_M_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_M_IGD_1:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_S_IGD:
	case PCI_PRODUCT_INTEL_IVYBRIDGE_S_IGD_1:
		isc->chiptype = CHIP_SNB;
		break;
	}

	switch (isc->chiptype) {
	case CHIP_I915:
	case CHIP_G33:
		apbase = AGP_I915_GMADR;
		break;
	case CHIP_I965:
	case CHIP_G4X:
	case CHIP_SNB:
		apbase = AGP_I965_GMADR;
		break;
	default:
		apbase = AGP_I810_GMADR;
		break;
	}

	if (isc->chiptype == CHIP_I965 || isc->chiptype == CHIP_G4X ||
	    isc->chiptype == CHIP_SNB) {
		error = agp_i965_map_aperture(&isc->vga_pa, sc, apbase);
	} else {
		error = agp_map_aperture(&isc->vga_pa, sc, apbase);
	}
	if (error != 0) {
		aprint_error(": can't map aperture\n");
		free(isc, M_AGP);
		return error;
	}

	if (isc->chiptype == CHIP_I915 || isc->chiptype == CHIP_G33) {
		error = pci_mapreg_map(&isc->vga_pa, AGP_I915_MMADR,
		    PCI_MAPREG_TYPE_MEM, 0, &isc->bst, &isc->bsh,
		    &mmadr, &mmadrsize);
		if (error != 0) {
			aprint_error(": can't map mmadr registers\n");
			agp_generic_detach(sc);
			return error;
		}
		error = pci_mapreg_map(&isc->vga_pa, AGP_I915_GTTADR,
		    PCI_MAPREG_TYPE_MEM, 0, &isc->gtt_bst, &isc->gtt_bsh,
		    NULL, NULL);
		if (error != 0) {
			aprint_error(": can't map gttadr registers\n");
			/* XXX we should release mmadr here */
			agp_generic_detach(sc);
			return error;
		}
	} else if (isc->chiptype == CHIP_I965 || isc->chiptype == CHIP_G4X ||
	    isc->chiptype == CHIP_SNB) {
		error = pci_mapreg_map(&isc->vga_pa, AGP_I965_MMADR,
		    PCI_MAPREG_TYPE_MEM, 0, &isc->bst, &isc->bsh,
		    &mmadr, &mmadrsize);
		if (error != 0) {
			aprint_error(": can't map mmadr registers\n");
			agp_generic_detach(sc);
			return error;
		}
	} else {
		error = pci_mapreg_map(&isc->vga_pa, AGP_I810_MMADR,
		    PCI_MAPREG_TYPE_MEM, 0, &isc->bst, &isc->bsh,
		    &mmadr, &mmadrsize);
		if (error != 0) {
			aprint_error(": can't map mmadr registers\n");
			agp_generic_detach(sc);
			return error;
		}
	}

	isc->initial_aperture = AGP_GET_APERTURE(sc);

	gatt = malloc(sizeof(struct agp_gatt), M_AGP, M_NOWAIT);
	if (!gatt) {
 		agp_generic_detach(sc);
 		return ENOMEM;
	}
	isc->gatt = gatt;

	gatt->ag_entries = AGP_GET_APERTURE(sc) >> AGP_PAGE_SHIFT;

	if (!pmf_device_register(self, NULL, agp_i810_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");

	/*
	 * XXX horrible hack to allow drm code to use our mapping
	 * of VGA chip registers
	 */
	agp_i810_vga_mapped = true;
	agp_i810_vga_regbase = mmadr;
	agp_i810_vga_bst = isc->bst;
	agp_i810_vga_bsh = isc->bsh;

	error = agp_i810_init(sc);
	if (error != 0) {
		free(gatt, M_AGP);
		agp_generic_detach(sc);
	}
	return error;
}

/*
 * XXX horrible hack to allow drm code to use our mapping
 * of VGA chip registers
 */
int
agp_i810_borrow(bus_addr_t base, bus_space_tag_t *tagp, bus_space_handle_t *hdlp)
{

	if (!agp_i810_vga_mapped || !agp_i810_vga_regbase ||
	    base != agp_i810_vga_regbase)
		return 0;
	*tagp = agp_i810_vga_bst;
	*hdlp = agp_i810_vga_bsh;
	return 1;
}

static int agp_i810_init(struct agp_softc *sc)
{
	struct agp_i810_softc *isc;
	struct agp_gatt *gatt;
	bus_addr_t tmp;
	int dummyrseg;

	isc = sc->as_chipc;
	gatt = isc->gatt;

	if (isc->chiptype == CHIP_I810) {
		void *virtual;
		int dummyseg;

		/* Some i810s have on-chip memory called dcache */
		if (READ1(AGP_I810_DRT) & AGP_I810_DRT_POPULATED)
			isc->dcache_size = 4 * 1024 * 1024;
		else
			isc->dcache_size = 0;

		/* According to the specs the gatt on the i810 must be 64k */
		if (agp_alloc_dmamem(sc->as_dmat, 64 * 1024,
		    0, &gatt->ag_dmamap, &virtual, &gatt->ag_physical,
		    &gatt->ag_dmaseg, 1, &dummyseg) != 0) {
			return ENOMEM;
		}
		gatt->ag_virtual = (uint32_t *)virtual;
		gatt->ag_size = gatt->ag_entries * sizeof(u_int32_t);
		memset(gatt->ag_virtual, 0, gatt->ag_size);

		agp_flush_cache();
		/* Install the GATT. */
		WRITE4(AGP_I810_PGTBL_CTL, gatt->ag_physical | 1);
	} else if (isc->chiptype == CHIP_I830) {
		/* The i830 automatically initializes the 128k gatt on boot. */
		pcireg_t reg;
		u_int32_t pgtblctl;
		u_int16_t gcc1;

		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I830_GCC0);
		gcc1 = (u_int16_t)(reg >> 16);
		switch (gcc1 & AGP_I830_GCC1_GMS) {
		case AGP_I830_GCC1_GMS_STOLEN_512:
			isc->stolen = (512 - 132) * 1024 / 4096;
			break;
		case AGP_I830_GCC1_GMS_STOLEN_1024:
			isc->stolen = (1024 - 132) * 1024 / 4096;
			break;
		case AGP_I830_GCC1_GMS_STOLEN_8192:
			isc->stolen = (8192 - 132) * 1024 / 4096;
			break;
		default:
			isc->stolen = 0;
			aprint_error(
			    ": unknown memory configuration, disabling\n");
			return EINVAL;
		}

		if (isc->stolen > 0) {
			aprint_normal(": detected %dk stolen memory\n%s",
			    isc->stolen * 4, device_xname(sc->as_dev));
		}

		/* GATT address is already in there, make sure it's enabled */
		pgtblctl = READ4(AGP_I810_PGTBL_CTL);
		pgtblctl |= 1;
		WRITE4(AGP_I810_PGTBL_CTL, pgtblctl);

		gatt->ag_physical = pgtblctl & ~1;
	} else if (isc->chiptype == CHIP_I855 || isc->chiptype == CHIP_I915 ||
		   isc->chiptype == CHIP_I965 || isc->chiptype == CHIP_G33 ||
		   isc->chiptype == CHIP_G4X) {
		pcireg_t reg;
		u_int32_t pgtblctl, gtt_size, stolen;
		u_int16_t gcc1;

		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I855_GCC1);
		gcc1 = (u_int16_t)(reg >> 16);

		pgtblctl = READ4(AGP_I810_PGTBL_CTL);

		/* Stolen memory is set up at the beginning of the aperture by
                 * the BIOS, consisting of the GATT followed by 4kb for the
		 * BIOS display.
                 */
                switch (isc->chiptype) {
		case CHIP_I855:
			gtt_size = 128;
			break;
                case CHIP_I915:
			gtt_size = 256;
			break;
		case CHIP_I965:
			switch (pgtblctl & AGP_I810_PGTBL_SIZE_MASK) {
			case AGP_I810_PGTBL_SIZE_128KB:
			case AGP_I810_PGTBL_SIZE_512KB:
				gtt_size = 512;
				break;
			case AGP_I965_PGTBL_SIZE_1MB:
				gtt_size = 1024;
				break;
			case AGP_I965_PGTBL_SIZE_2MB:
				gtt_size = 2048;
				break;
			case AGP_I965_PGTBL_SIZE_1_5MB:
				gtt_size = 1024 + 512;
				break;
			default:
				aprint_error("Bad PGTBL size\n");
				return EINVAL;
			}
			break;
		case CHIP_G33:
			switch (gcc1 & AGP_G33_PGTBL_SIZE_MASK) {
			case AGP_G33_PGTBL_SIZE_1M:
				gtt_size = 1024;
				break;
			case AGP_G33_PGTBL_SIZE_2M:
				gtt_size = 2048;
				break;
			default:
				aprint_error(": Bad PGTBL size\n");
				return EINVAL;
			}
			break;
		case CHIP_G4X:
			gtt_size = 0;
			break;
		default:
			aprint_error(": bad chiptype\n");
			return EINVAL;
		}

		switch (gcc1 & AGP_I855_GCC1_GMS) {
		case AGP_I855_GCC1_GMS_STOLEN_1M:
			stolen = 1024;
			break;
		case AGP_I855_GCC1_GMS_STOLEN_4M:
			stolen = 4 * 1024;
			break;
		case AGP_I855_GCC1_GMS_STOLEN_8M:
			stolen = 8 * 1024;
			break;
		case AGP_I855_GCC1_GMS_STOLEN_16M:
			stolen = 16 * 1024;
			break;
		case AGP_I855_GCC1_GMS_STOLEN_32M:
			stolen = 32 * 1024;
			break;
		case AGP_I915_GCC1_GMS_STOLEN_48M:
			stolen = 48 * 1024;
			break;
		case AGP_I915_GCC1_GMS_STOLEN_64M:
			stolen = 64 * 1024;
			break;
		case AGP_G33_GCC1_GMS_STOLEN_128M:
			stolen = 128 * 1024;
			break;
		case AGP_G33_GCC1_GMS_STOLEN_256M:
			stolen = 256 * 1024;
			break;
		case AGP_G4X_GCC1_GMS_STOLEN_96M:
			stolen = 96 * 1024;
			break;
		case AGP_G4X_GCC1_GMS_STOLEN_160M:
			stolen = 160 * 1024;
			break;
		case AGP_G4X_GCC1_GMS_STOLEN_224M:
			stolen = 224 * 1024;
			break;
		case AGP_G4X_GCC1_GMS_STOLEN_352M:
			stolen = 352 * 1024;
			break;
		default:
			aprint_error(
			    ": unknown memory configuration, disabling\n");
			return EINVAL;
		}

		switch (gcc1 & AGP_I855_GCC1_GMS) {
		case AGP_I915_GCC1_GMS_STOLEN_48M:
		case AGP_I915_GCC1_GMS_STOLEN_64M:
			if (isc->chiptype != CHIP_I915 &&
			    isc->chiptype != CHIP_I965 &&
			    isc->chiptype != CHIP_G33 &&
			    isc->chiptype != CHIP_G4X)
				stolen = 0;
			break;
		case AGP_G33_GCC1_GMS_STOLEN_128M:
		case AGP_G33_GCC1_GMS_STOLEN_256M:
			if (isc->chiptype != CHIP_I965 &&
			    isc->chiptype != CHIP_G33 &&
			    isc->chiptype != CHIP_G4X)
				stolen = 0;
			break;
		case AGP_G4X_GCC1_GMS_STOLEN_96M:
		case AGP_G4X_GCC1_GMS_STOLEN_160M:
		case AGP_G4X_GCC1_GMS_STOLEN_224M:
		case AGP_G4X_GCC1_GMS_STOLEN_352M:
			if (isc->chiptype != CHIP_I965 &&
			    isc->chiptype != CHIP_G4X)
				stolen = 0;
			break;
		}

		/* BIOS space */
		gtt_size += 4;

		isc->stolen = (stolen - gtt_size) * 1024 / 4096;

		if (isc->stolen > 0) {
			aprint_normal(": detected %dk stolen memory\n%s",
			    isc->stolen * 4, device_xname(sc->as_dev));
		}

		/* GATT address is already in there, make sure it's enabled */
		pgtblctl |= 1;
		WRITE4(AGP_I810_PGTBL_CTL, pgtblctl);

		gatt->ag_physical = pgtblctl & ~1;
	} else if (isc->chiptype == CHIP_SNB) {
		/*
		 * Sandy Bridge has new memory control reg at 0x50.
		 */
		u_int32_t stolen;
		u_int16_t gcc1;

		gcc1 = (u_int16_t)pci_conf_read(sc->as_pc, sc->as_tag,
		    AGP_SNB_GMCH_CTRL);

		stolen = 4;

		switch (gcc1 & AGP_SNB_GMCH_GMS_STOLEN_MASK) {
		case AGP_SNB_GMCH_GMS_STOLEN_32M:
			isc->stolen = (32768 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_64M:
			isc->stolen = (65536 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_96M:
			isc->stolen = (98304 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_128M:
			isc->stolen = (131072 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_160M:
			isc->stolen = (163840 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_192M:
			isc->stolen = (196608 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_224M:
			isc->stolen = (229376 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_256M:
			isc->stolen = (262144 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_288M:
			isc->stolen = (294912 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_320M:
			isc->stolen = (327680 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_352M:
			isc->stolen = (360448 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_384M:
			isc->stolen = (393216 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_416M:
			isc->stolen = (425984 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_448M:
			isc->stolen = (458752 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_480M:
			isc->stolen = (491520 - stolen) * 1024 / 4096;
			break;
		case AGP_SNB_GMCH_GMS_STOLEN_512M:
			isc->stolen = (524288 - stolen) * 1024 / 4096;
			break;
		default:
			isc->stolen = 0;
			aprint_error(
			    ": unknown memory configuration, disabling\n");
			return EINVAL;
		}

		if (isc->stolen > 0)
			aprint_normal(": detected %dk stolen memory\n%s",
			    isc->stolen * 4, device_xname(sc->as_dev));
		else
			aprint_normal(": no pre-allocated video memory\n%s",
			    device_xname(sc->as_dev));

		/* GATT address is already in there, make sure it's enabled */
		gatt->ag_physical = READ4(AGP_I810_PGTBL_CTL) & ~1;
	} else {
		aprint_error(": Unknown chipset\n");
		return EINVAL;
	}

	/* Intel recommends that you have a fake page bound to the gtt always */
	if (agp_alloc_dmamem(sc->as_dmat, AGP_PAGE_SIZE, 0, &isc->scrib_dmamap,
	    &isc->scrib_vaddr, &tmp, &isc->scrib_seg, 1, &dummyrseg) != 0) {
		aprint_error(": can't get scribble page\n");
		return ENOMEM;
	}

	tmp = 0;
	if (isc->chiptype == CHIP_I810) {
		tmp += isc->dcache_size;
	} else {
		tmp += isc->stolen << AGP_PAGE_SHIFT;
	}

	/* initialise all gtt entries to point to scribble page */
	for (; tmp < sc->as_apsize; tmp += AGP_PAGE_SIZE)
		agp_i810_unbind_page(sc, tmp);
	/* XXX we'll need to restore the GTT contents when we go kms */

	/*
	 * Make sure the chipset can see everything.
	 */
	agp_flush_cache();

	/*
	 * Publish what we found for kludgey drivers (I'm looking at
	 * you, drm).
	 */
	if (agp_i810_sc == NULL)
		agp_i810_sc = sc;
	else
		aprint_error_dev(sc->as_dev, "i810 agp already attached\n");

	return 0;
}

#if 0
static int
agp_i810_detach(struct agp_softc *sc)
{
	int error;
	struct agp_i810_softc *isc = sc->as_chipc;

	error = agp_generic_detach(sc);
	if (error)
		return error;

	/* Clear the GATT base. */
	if (sc->chiptype == CHIP_I810) {
		WRITE4(AGP_I810_PGTBL_CTL, 0);
	} else {
		unsigned int pgtblctl;
		pgtblctl = READ4(AGP_I810_PGTBL_CTL);
		pgtblctl &= ~1;
		WRITE4(AGP_I810_PGTBL_CTL, pgtblctl);
	}
	agp_free_dmamem(sc->as_dmat, AGP_PAGE_SIZE, isc->scrib_dmamap,
	    isc->scrib_vaddr, &isc->scrib_seg, 1);

	/* Put the aperture back the way it started. */
	AGP_SET_APERTURE(sc, isc->initial_aperture);

	if (sc->chiptype == CHIP_I810) {
		agp_free_dmamem(sc->as_dmat, gatt->ag_size, gatt->ag_dmamap,
		    (void *)gatt->ag_virtual, &gatt->ag_dmaseg, 1);
	}
	free(sc->gatt, M_AGP);

	return 0;
}
#endif

/*
 * XXX Using MSAC (on i915 and later) to determine the aperture size seems
 * XXX wrong.  Indeed, according to Intel docs (e.g., [1, page 175]),
 * XXX
 * XXX "Only the system BIOS will write this register based on pre-boot
 * XXX  address allocation efforts, but the graphics may read this register
 * XXX  to determine the correct aperture size."
 * XXX
 * XXX For instance, for i965, if MSAC bits [2:1] are 01, then the memory BAR
 * XXX GMADR cannot be of length 128MB, since its bit [27] is forced to be 0.
 * XXX In that case, GMADR may be of length 256MB or 512MB.
 * XXX
 * XXX Experiments on a laptop with an Ironlake IGD give MSAC bits [2:1] that
 * XXX are equal to 00, meaning 128MB or more, whereas both X.org and Linux
 * XXX report an aperture of 256MB.
 * XXX
 * XXX [1] http://intellinuxgraphics.org/VOL_1_graphics_core.pdf
 */
static u_int32_t
agp_i810_get_aperture(struct agp_softc *sc)
{
	struct agp_i810_softc *isc = sc->as_chipc;
	pcireg_t reg;
	u_int32_t size;
	u_int16_t miscc, gcc1;
	u_int8_t msac;

	size = 0;

	switch (isc->chiptype) {
	case CHIP_I810:
		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I810_SMRAM);
		miscc = (u_int16_t)(reg >> 16);
		if ((miscc & AGP_I810_MISCC_WINSIZE) ==
		    AGP_I810_MISCC_WINSIZE_32)
			size = 32 * 1024 * 1024;
		else
			size = 64 * 1024 * 1024;
		break;
	case CHIP_I830:
		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I830_GCC0);
		gcc1 = (u_int16_t)(reg >> 16);
		if ((gcc1 & AGP_I830_GCC1_GMASIZE) == AGP_I830_GCC1_GMASIZE_64)
			size = 64 * 1024 * 1024;
		else
			size = 128 * 1024 * 1024;
		break;
	case CHIP_I855:
		size = 128 * 1024 * 1024;
		break;
	case CHIP_I915:
		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I915_MSAC);
		msac = (u_int8_t)(reg >> 16);
		if (msac & AGP_I915_MSAC_APER_128M)
			size = 128 * 1024 * 1024;
		else
			size = 256 * 1024 * 1024;
		break;
	case CHIP_G33:
	case CHIP_G4X:
	case CHIP_SNB:
#if 0		/* XXX See above comment regarding MSAC. */
		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I965_MSAC);
		msac = (u_int8_t)(reg >> 16);
		switch (msac & AGP_I965_MSAC_APER_MASK) {
		case AGP_I965_MSAC_APER_128M:
			size = 128 * 1024 * 1024;
			break;
		case AGP_I965_MSAC_APER_256M:
			size = 256 * 1024 * 1024;
			break;
		case AGP_I965_MSAC_APER_512M:
			size = 512 * 1024 * 1024;
			break;
		default:
			aprint_error_dev(sc->as_dev, "invalid aperture size\n");
		}
#else
		size = sc->as_apsize;
#endif
		break;
	case CHIP_I965:
		/* XXX Why not use the same logic as above? */
		size = 512 * 1024 * 1024;
		break;
	default:
		aprint_error(": Unknown chipset\n");
	}

	return size;
}

static int
agp_i810_set_aperture(struct agp_softc *sc, u_int32_t aperture)
{
	struct agp_i810_softc *isc = sc->as_chipc;
	pcireg_t reg;
	u_int16_t miscc, gcc1;

	switch (isc->chiptype) {
	case CHIP_I810:
		/*
		 * Double check for sanity.
		 */
		if (aperture != (32 * 1024 * 1024) &&
		    aperture != (64 * 1024 * 1024)) {
			aprint_error_dev(sc->as_dev, "bad aperture size %d\n",
			    aperture);
			return EINVAL;
		}

		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I810_SMRAM);
		miscc = (u_int16_t)(reg >> 16);
		miscc &= ~AGP_I810_MISCC_WINSIZE;
		if (aperture == 32 * 1024 * 1024)
			miscc |= AGP_I810_MISCC_WINSIZE_32;
		else
			miscc |= AGP_I810_MISCC_WINSIZE_64;

		reg &= 0x0000ffff;
		reg |= ((pcireg_t)miscc) << 16;
		pci_conf_write(sc->as_pc, sc->as_tag, AGP_I810_SMRAM, reg);
		break;
	case CHIP_I830:
		if (aperture != (64 * 1024 * 1024) &&
		    aperture != (128 * 1024 * 1024)) {
			aprint_error_dev(sc->as_dev, "bad aperture size %d\n",
			    aperture);
			return EINVAL;
		}
		reg = pci_conf_read(sc->as_pc, sc->as_tag, AGP_I830_GCC0);
		gcc1 = (u_int16_t)(reg >> 16);
		gcc1 &= ~AGP_I830_GCC1_GMASIZE;
		if (aperture == 64 * 1024 * 1024)
			gcc1 |= AGP_I830_GCC1_GMASIZE_64;
		else
			gcc1 |= AGP_I830_GCC1_GMASIZE_128;

		reg &= 0x0000ffff;
		reg |= ((pcireg_t)gcc1) << 16;
		pci_conf_write(sc->as_pc, sc->as_tag, AGP_I830_GCC0, reg);
		break;
	case CHIP_I855:
	case CHIP_I915:
	case CHIP_I965:
	case CHIP_G33:
	case CHIP_G4X:
	case CHIP_SNB:
		if (aperture != agp_i810_get_aperture(sc)) {
			aprint_error_dev(sc->as_dev, "bad aperture size %d\n",
			    aperture);
			return EINVAL;
		}
		break;
	default:
		aprint_error(": Unknown chipset\n");
	}

	return 0;
}

static int
agp_i810_bind_page(struct agp_softc *sc, off_t offset, bus_addr_t physical,
    int flags)
{
	struct agp_i810_softc *isc = sc->as_chipc;

	if (offset < 0 || offset >= (isc->gatt->ag_entries << AGP_PAGE_SHIFT)) {
#ifdef AGP_DEBUG
		printf("%s: failed: offset 0x%08x, shift %d, entries %d\n",
		    device_xname(sc->as_dev), (int)offset, AGP_PAGE_SHIFT,
		    isc->gatt->ag_entries);
#endif
		return EINVAL;
	}

	if (isc->chiptype != CHIP_I810) {
		if ((offset >> AGP_PAGE_SHIFT) < isc->stolen) {
#ifdef AGP_DEBUG
			printf("%s: trying to bind into stolen memory: 0x%08x\n",
			    device_xname(sc->as_dev), (int)offset);
#endif
			return EINVAL;
		}
	}

	/*
	 * COHERENT mappings mean set the snoop bit. this should never be
	 * accessed by the gpu through the gtt.
	 */
	if (flags & BUS_DMA_COHERENT)
		physical |= INTEL_COHERENT;

	return agp_i810_write_gtt_entry(isc, offset, physical | 1);
}

static int
agp_i810_unbind_page(struct agp_softc *sc, off_t offset)
{
	struct agp_i810_softc *isc = sc->as_chipc;

	if (offset < 0 || offset >= (isc->gatt->ag_entries << AGP_PAGE_SHIFT)) {
#ifdef AGP_DEBUG
		printf("%s: failed: offset 0x%08x, shift %d, entries %d\n",
		    device_xname(sc->as_dev), (int)offset, AGP_PAGE_SHIFT,
		    isc->gatt->ag_entries);
#endif
		return EINVAL;
	}

	if (isc->chiptype != CHIP_I810 ) {
		if ((offset >> AGP_PAGE_SHIFT) < isc->stolen) {
#ifdef AGP_DEBUG
			printf("%s: trying to unbind from stolen memory: 0x%08x\n",
			    device_xname(sc->as_dev), (int)offset);
#endif
			return EINVAL;
		}
	}

	return agp_i810_write_gtt_entry(isc, offset,
	    isc->scrib_dmamap->dm_segs[0].ds_addr | 1);
}

/*
 * Writing via memory mapped registers already flushes all TLBs.
 */
static void
agp_i810_flush_tlb(struct agp_softc *sc)
{
}

static int
agp_i810_enable(struct agp_softc *sc, u_int32_t mode)
{

	return 0;
}

static struct agp_memory *
agp_i810_alloc_memory(struct agp_softc *sc, int type, vsize_t size)
{
	struct agp_i810_softc *isc = sc->as_chipc;
	struct agp_memory *mem;

#ifdef AGP_DEBUG
	printf("AGP: alloc(%d, 0x%x)\n", type, (int) size);
#endif

	if ((size & (AGP_PAGE_SIZE - 1)) != 0)
		return 0;

	if (sc->as_allocated + size > sc->as_maxmem)
		return 0;

	if (type == 1) {
		/*
		 * Mapping local DRAM into GATT.
		 */
		if (isc->chiptype != CHIP_I810 )
			return 0;
		if (size != isc->dcache_size)
			return 0;
	} else if (type == 2) {
		/*
		 * Bogus mapping for the hardware cursor.
		 */
		if (size != AGP_PAGE_SIZE && size != 4 * AGP_PAGE_SIZE)
			return 0;
	}

	mem = malloc(sizeof *mem, M_AGP, M_WAITOK|M_ZERO);
	if (mem == NULL)
		return NULL;
	mem->am_id = sc->as_nextid++;
	mem->am_size = size;
	mem->am_type = type;

	if (type == 2) {
		/*
		 * Allocate and wire down the memory now so that we can
		 * get its physical address.
		 */
		mem->am_dmaseg = malloc(sizeof *mem->am_dmaseg, M_AGP,
		    M_WAITOK);
		if (mem->am_dmaseg == NULL) {
			free(mem, M_AGP);
			return NULL;
		}
		if (agp_alloc_dmamem(sc->as_dmat, size, 0,
		    &mem->am_dmamap, &mem->am_virtual, &mem->am_physical,
		    mem->am_dmaseg, 1, &mem->am_nseg) != 0) {
			free(mem->am_dmaseg, M_AGP);
			free(mem, M_AGP);
			return NULL;
		}
		memset(mem->am_virtual, 0, size);
	} else if (type != 1) {
		if (bus_dmamap_create(sc->as_dmat, size, size / PAGE_SIZE + 1,
				      size, 0, BUS_DMA_NOWAIT,
				      &mem->am_dmamap) != 0) {
			free(mem, M_AGP);
			return NULL;
		}
	}

	TAILQ_INSERT_TAIL(&sc->as_memory, mem, am_link);
	sc->as_allocated += size;

	return mem;
}

static int
agp_i810_free_memory(struct agp_softc *sc, struct agp_memory *mem)
{
	if (mem->am_is_bound)
		return EBUSY;

	if (mem->am_type == 2) {
		agp_free_dmamem(sc->as_dmat, mem->am_size, mem->am_dmamap,
		    mem->am_virtual, mem->am_dmaseg, mem->am_nseg);
		free(mem->am_dmaseg, M_AGP);
	}

	sc->as_allocated -= mem->am_size;
	TAILQ_REMOVE(&sc->as_memory, mem, am_link);
	free(mem, M_AGP);
	return 0;
}

static int
agp_i810_bind_memory(struct agp_softc *sc, struct agp_memory *mem,
		     off_t offset)
{
	struct agp_i810_softc *isc = sc->as_chipc;
	u_int32_t regval, i;

	if (mem->am_is_bound != 0)
		return EINVAL;

	/*
	 * XXX evil hack: the PGTBL_CTL appearently gets overwritten by the
	 * X server for mysterious reasons which leads to crashes if we write
	 * to the GTT through the MMIO window.
	 * Until the issue is solved, simply restore it.
	 */
	regval = bus_space_read_4(isc->bst, isc->bsh, AGP_I810_PGTBL_CTL);
	if (regval != (isc->gatt->ag_physical | 1)) {
		printf("agp_i810_bind_memory: PGTBL_CTL is 0x%x - fixing\n",
		       regval);
		bus_space_write_4(isc->bst, isc->bsh, AGP_I810_PGTBL_CTL,
				  isc->gatt->ag_physical | 1);
	}

	if (mem->am_type == 2) {
		for (i = 0; i < mem->am_size; i += AGP_PAGE_SIZE)
			agp_i810_bind_page(sc, offset + i,
			    mem->am_physical + i, 0);
		mem->am_offset = offset;
		mem->am_is_bound = 1;
		return 0;
	}

	if (mem->am_type != 1)
		return agp_generic_bind_memory(sc, mem, offset);

	if (isc->chiptype != CHIP_I810)
		return EINVAL;

	for (i = 0; i < mem->am_size; i += AGP_PAGE_SIZE)
		agp_i810_write_gtt_entry(isc, i, i | 3);
	mem->am_is_bound = 1;
	return 0;
}

static int
agp_i810_unbind_memory(struct agp_softc *sc, struct agp_memory *mem)
{
	struct agp_i810_softc *isc = sc->as_chipc;
	u_int32_t i;

	if (mem->am_is_bound == 0)
		return EINVAL;

	if (mem->am_type == 2) {
		for (i = 0; i < mem->am_size; i += AGP_PAGE_SIZE)
			agp_i810_unbind_page(sc, mem->am_offset + i);
		mem->am_offset = 0;
		mem->am_is_bound = 0;
		return 0;
	}

	if (mem->am_type != 1)
		return agp_generic_unbind_memory(sc, mem);

	if (isc->chiptype != CHIP_I810)
		return EINVAL;

	for (i = 0; i < mem->am_size; i += AGP_PAGE_SIZE)
		agp_i810_write_gtt_entry(isc, i, 0);
	mem->am_is_bound = 0;
	return 0;
}

static bool
agp_i810_resume(device_t dv, const pmf_qual_t *qual)
{
	struct agp_softc *sc = device_private(dv);
	struct agp_i810_softc *isc = sc->as_chipc;

	isc->pgtblctl = READ4(AGP_I810_PGTBL_CTL);
	agp_flush_cache();

	return true;
}
