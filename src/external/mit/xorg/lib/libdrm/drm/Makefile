#	$NetBSD: Makefile,v 1.5 2010/05/22 23:19:24 mrg Exp $

# install include/libdrm files

.include <bsd.own.mk>

.PATH:	${NETBSDSRCDIR}/sys/dev/pci/drm

INCS=	drm.h \
	drm_sarea.h \
	i915_drm.h

INCSDIR=${X11INCDIR}/libdrm

.include <bsd.x11.mk>
.include <bsd.prog.mk>
