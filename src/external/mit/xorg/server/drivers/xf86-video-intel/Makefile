#	$NetBSD: Makefile,v 1.10 2011/08/11 23:15:40 joerg Exp $

.include <bsd.own.mk>

DRIVER=		xf86-video-intel
DRIVER_NAME=	intel_drv

SRCS=		intel_module.c intel_driver.c intel_batchbuffer.c
SRCS+=		intel_memory.c intel_shadow.c intel_uxa.c intel_video.c
SRCS+=		i830_3d.c i830_bios.c i830_crt.c i830_cursor.c
SRCS+=		i830_display.c i830_quirks.c i830_dvo.c i830_dp.c i830_hdmi.c
SRCS+=		i830_i2c.c i830_io.c i830_lvds.c i830_modes.c
SRCS+=		i830_render.c i830_sdvo.c i830_tv.c i915_3d.c
SRCS+=		i915_render.c i915_video.c i965_3d.c i965_video.c
SRCS+=		i965_render.c drmmode_display.c
MAN=		intel.4

# Source files of local libraries
SRCS.uxa=	uxa.c uxa-accel.c uxa-glyphs.c uxa-render.c uxa-unaccel.c
SRCS.legacy=	i810_accel.c i810_cursor.c i810_dga.c i810_driver.c
SRCS.legacy+=	i810_io.c i810_memory.c i810_video.c i810_wmark.c

# DRI support
.if ${X11DRI} != "no"
SRCS+=		intel_dri.c
SRCS.legacy+=	i810_dri.c
CPPFLAGS+=	-DXF86DRI
.endif

# Enable XvMC
SRCS+=		intel_hwmc.c
SRCS.legacy+=	i810_hwmc.c
CPPFLAGS+=	-DENABLE_XVMC

# Local libraries
SRCS+=		$(SRCS.uxa) $(SRCS.legacy)
.PATH:	${X11SRCDIR.${DRIVER}}/uxa
.PATH:	${X11SRCDIR.${DRIVER}}/src/legacy/i810

CPPFLAGS+=	-I${DESTDIR}${X11INCDIR}/libdrm
CPPFLAGS+=	-I${X11SRCDIR.${DRIVER}}/uxa
CPPFLAGS+=	-I${X11SRCDIR.${DRIVER}}/src
CPPFLAGS+=	-I${X11SRCDIR.${DRIVER}}/src/render_program

LDADD+=		-lm -ldrm -ldrm_intel -lpciaccess

.include "../Makefile.xf86-driver"

SUBDIR=	ch7017 ch7xxx ivch sil164 tfp410

CWARNFLAGS.clang+=	-Wno-parentheses -Wno-tautological-compare \
			-Wno-empty-body -Wno-error

.include <bsd.subdir.mk>
