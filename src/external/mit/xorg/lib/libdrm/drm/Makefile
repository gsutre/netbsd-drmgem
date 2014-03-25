#	$NetBSD: Makefile,v 1.6 2014/03/17 08:01:18 mrg Exp $

# install include/libdrm files

.include <bsd.own.mk>

.PATH:	${NETBSDSRCDIR}/sys/dev/pci/drm

INCS=	drm.h \
	drm_mode.h \
	drm_sarea.h \
	i915_drm.h

INCSDIR=${X11INCDIR}/libdrm

.include <bsd.x11.mk>
.include <bsd.prog.mk>
