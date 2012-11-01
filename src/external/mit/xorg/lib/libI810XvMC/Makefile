#	$NetBSD: Makefile,v 1.2 2011/01/19 01:52:44 jakllsch Exp $

.include <bsd.own.mk>

LIB=	I810XvMC
.PATH:	${X11SRCDIR.xf86-video-intel}/src/legacy/i810/xvmc

SRCS=	I810XvMC.c

CPPFLAGS+=	${X11FLAGS.THREADLIB} \
		-DHAVE_XEXTPROTO_71 \
		-DXSERVER_LIBPCIACCESS \
		-DTRUE=1 -DFALSE=0
CPPFLAGS+=	-I${DESTDIR}${X11INCDIR}/libdrm \
		-I${DESTDIR}${X11INCDIR}/xorg \
		-I${X11SRCDIR.xf86-video-intel}/src

LIBDPLIBS=	Xext	${.CURDIR}/../libXext \
		Xfixes	${.CURDIR}/../libXfixes \
		X11	${.CURDIR}/../libX11/dynamic \
		X11-xcb	${.CURDIR}/../libX11/libX11-xcb \
		Xv	${.CURDIR}/../libXv \
		XvMC	${.CURDIR}/../libXvMC \
		xcb	${.CURDIR}/../libxcb/libxcb \
		xcb-dri2	${.CURDIR}/../libxcb/dri2 \
		xcb-aux	${.CURDIR}/../xcb-util/aux \
		drm	${.CURDIR}/../libdrm

.include <bsd.x11.mk>
.include <bsd.lib.mk>
