#	$NetBSD: Makefile,v 1.28 2014/03/17 07:25:34 mrg Exp $

# build libraries for xorg

SUBDIR=	libfontenc freetype libFS libICE libOSMesa \
	libXau libXdmcp libdrm libpciaccess libpthread-stubs .WAIT
.if ${MACHINE_ARCH} == "i386" || ${MACHINE_ARCH} == "x86_64"
SUBDIR+=libdrm_intel
.endif
#SUBDIR+=libdrm_radeon
SUBDIR+=pixman xkeyboard-config libxcb \
	.WAIT \
	dri fontconfig libSM libX11 \
	.WAIT \
	libXcomposite libXdamage libXext libXfixes libXt \
	libxkbfile \
	.WAIT \
	libXi libXxf86vm \
	.WAIT \
	libGL libXScrnSaver libXTrap libXevie libXfont libXfontcache \
	libXinerama libXmu libXmuu libXpm libXrender \
	libXres libXtst libXvMC libXvMCW libXv libXxf86dga libXxf86misc \
	libxkbui \
	xcb-util \
	.WAIT
.if ${MACHINE_ARCH} == "i386" || ${MACHINE_ARCH} == "x86_64"
SUBDIR+=libI810XvMC
SUBDIR+=libIntelXvMC
#SUBDIR+=libchromeXvMC
#SUBDIR+=libchromeXvMCPro
.endif
SUBDIR+=libGLU libGLw libXaw6 libXaw libXcursor \
	libXrandr \
	.WAIT \
	libXft libglut \
	.WAIT \
	libXdmGreet
	
.include <bsd.subdir.mk>
