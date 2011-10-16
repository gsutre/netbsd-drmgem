/*	$OpenBSD: bus.h,v 1.24 2010/09/06 19:05:48 kettenis Exp $	*/
/*	$NetBSD: bus.h,v 1.6 1996/11/10 03:19:25 thorpej Exp $	*/

/*-
 * Copyright (c) 1996, 1997 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe of the Numerical Aerospace Simulation Facility,
 * NASA Ames Research Center.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1996 Charles M. Hannum.  All rights reserved.
 * Copyright (c) 1996 Jason R. Thorpe.  All rights reserved.
 * Copyright (c) 1996 Christopher G. Demetriou.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Christopher G. Demetriou
 *	for the NetBSD Project.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _X86_SG_DMA_H_
#define _X86_SG_DMA_H_

#include <sys/types.h>
#include <sys/extent.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/tree.h>

#include <sys/bus.h>

/* Scatter gather bus_dma functions. */
struct sg_cookie {
	kmutex_t	 sg_mtx;
	struct extent	*sg_ex;
	void		*sg_hdl;
	struct bus_dma_overrides	sg_ov;
	bus_dma_tag_t	sg_dmat;

	void		(*bind_page)(void *, bus_addr_t, paddr_t, int);
	void		(*unbind_page)(void *, bus_addr_t);
	void		(*flush_tlb)(void *);
};

/*
 * per-map DVMA page table
 */
struct sg_page_entry {
	SPLAY_ENTRY(sg_page_entry)	spe_node;
	paddr_t				spe_pa;
	bus_addr_t			spe_va;
};

/* for sg_dma this will be in the map's dm_sg_cookie. */
struct sg_page_map {
	SPLAY_HEAD(sg_page_tree, sg_page_entry) spm_tree;

	void			*spm_origbuf;	/* pointer to original data */
	int			 spm_buftype;	/* type of data */
	struct proc		*spm_proc;	/* proc that owns the mapping */

	int			 spm_maxpage;	/* Size of allocated page map */
	int			 spm_pagecnt;	/* Number of entries in use */
	bus_addr_t		 spm_start;	/* dva when bound */
	bus_size_t		 spm_size;	/* size of bound map */
	struct sg_page_entry	 spm_map[1];
};

int	sg_dmatag_create(const char *, void *, bus_dma_tag_t,
	    bus_addr_t, bus_size_t,
	    void (*)(void *, bus_addr_t, paddr_t, int),
	    void (*)(void *, bus_addr_t), void (*)(void *),
	    void (*)(void *, bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
		bus_size_t, int),
	    bus_dma_tag_t *);
void	sg_dmatag_destroy(struct sg_cookie *);
void	sg_dmamap_set_alignment(bus_dma_tag_t, bus_dmamap_t, u_long);

#endif /* _X86_SG_DMA_H_ */
