#!/bin/sh
#
# Automated build script for netbsd-drmgem.  Needs a NetBSD CVS time stamp,
# for instance:
#
# autobuild.sh '2012-10-04 08:10 UTC'
#
# WARNING: this script modifies the local X11R7 installation!
#

# Cache this for message output
self="$(basename "$0")"

# Abort as soon as something wrong occurs
set -e

# Anonymous access to the NetBSD CVS repository (french mirror)
export CVSROOT=anoncvs@anoncvs.fr.NetBSD.org:/pub/NetBSD-CVS
export CVS_RSH=ssh

# Base URL for netbsd-drmgem tarball
GITHUB_TARBALL_URL=https://github.com/gsutre/netbsd-drmgem/tarball

# Use netbsd-drmgem's master branch by default (selectable by option)
BRANCH="master"

# Make command
MAKE="make"

# Download sources and extract them by default
continue="no"

# Don't build the intel DRI modules by default
mesa="no"

# Don't build the intel XvMC libraries by default
xvmc="no"


# Helper functions #############################################################

error ()
{
	echo "$self: error: $1" 1>&2
	exit 1
}

build_and_install ()
{
	cd "$1"
	sudo -E $MAKE includes
	$MAKE
	sudo -E $MAKE install
	cd ..
}


# Command-line Processing ######################################################

usage ()
{
	cat <<EOF
Usage: $self [-cmv] [-b <branch>] [-j <njobs>] <time stamp>
EOF
	exit 1
}

while getopts "b:cj:mv" option; do
	case $option in
		b)
			BRANCH="$OPTARG"
			;;
		c)
			continue="yes"
			;;
		j)
			MAKE="$MAKE -j $OPTARG"
			;;
		m)
			mesa="yes"
			;;
		v)
			xvmc="yes"
			;;
		*)
			usage
			;;
	esac
done

shift $(($OPTIND - 1))

TIMESTAMP="$1"

if [ -z "$TIMESTAMP" ]; then
	usage
fi

# Make a time stamp like 201210040810Z compatible with cvs
TIMESTAMP=$(echo "$TIMESTAMP" | \
	    sed -e 's/^\([0-9]\{8\}\)\([0-9]\{4\}Z\)$/\1 \2/g')


# Main #########################################################################

# No need for the full source tree, only these directories
SRC_FILES="				\
	src/build.sh 			\
	src/etc 			\
	src/external/mit 		\
	src/include 			\
	src/lib 			\
	src/tools"

SYSSRC_FILES="				\
	src/common 			\
	src/sys"

XSRC_FILES="				\
	xsrc/external/mit/MesaLib 	\
	xsrc/external/mit/libXau 	\
	xsrc/external/mit/libXdmcp 	\
	xsrc/external/mit/libxcb 	\
	xsrc/external/mit/libX11 	\
	xsrc/external/mit/libXext 	\
	xsrc/external/mit/libXv 	\
	xsrc/external/mit/libXvMC"

# Check for NetBSD 6.99
set -- $(uname -r | tr '.' ' ')
if [ "$(uname -s)" != "NetBSD" -o "$1" != "6" -o "$2" != "99" ]; then
	error "host should be running NetBSD 6.99."
fi
set --

if [ "$continue" != "yes" ]; then
	# Check remaining free space
	set -- $(df -m . | tail -1)
	if [ "$4" -lt "750" ]; then
		error "insufficient free space ($4 MiB < 750 MiB)."
	fi
	set --

	# Check for wget
	if ! which wget >/dev/null; then
		error "wget not found."
	fi
fi

# Check for sudo
if ! which sudo >/dev/null; then
	error "sudo not found."
fi

DISTDIR=dist.$(uname -m)

if [ "$continue" = "yes" ]; then
	if [ ! \( -d "$DISTDIR" -a -e ".startbuild" -a -d "usr" \) ]; then
		error "can't continue: missing file or directory."
	fi
else
	# Create distribution directory and time stamp for build start
	mkdir -p $DISTDIR
	touch .startbuild

	# Download and extract netbsd-drmgem
	wget --no-check-certificate -O - ${GITHUB_TARBALL_URL}/$BRANCH | tar -zxf -
	mv gsutre-netbsd-drmgem-* netbsd-drmgem

	# Get NetBSD sources by CVS
	for f in ${SRC_FILES} ${SYSSRC_FILES} ${XSRC_FILES}; do
		cvs -z3 export -N -d usr -D "$TIMESTAMP" "$f"
	done

	# Overwrite source files with netbsd-drmgem ones
	cp -av netbsd-drmgem/src netbsd-drmgem/xsrc usr
fi

export USETOOLS=no

# Build and install new kernel
cd usr/src/sys/arch/$(uname -m)/conf
config GENERIC
cd ../compile/GENERIC
$MAKE depend
$MAKE
mv netbsd ../../../../../../../$DISTDIR/netbsd-GENERIC-drmgem
cd ../../../../../../..

# Build the math library
$MAKE -C usr/src/lib/libm

# Build and install OpenBSD Xenocara's libdrm and Xorg intel driver
cd usr/src/external/mit/xorg/lib
build_and_install libdrm
build_and_install libdrm_intel
cd ../server/drivers
build_and_install xf86-video-intel
cd ../../../../../../..

if [ "$mesa" = "yes" ]; then
	# Build the intel DRI modules
	$MAKE -C usr/src/external/mit/xorg/tools/glsl
	$MAKE -C usr/src/external/mit/expat
	$MAKE -C usr/src/external/mit/xorg/lib/dri/libmesa
	cd usr/src/external/mit/xorg/lib/dri
	build_and_install i915
	build_and_install i965
	cd ../../../../../../..
fi

if [ "$xvmc" = "yes" ]; then
	# Build the intel XvMC libraries
	$MAKE -C usr/src/external/mit/xorg/lib/libXau
	$MAKE -C usr/src/external/mit/xorg/lib/libXdmcp
	$MAKE -C usr/src/external/mit/xorg/lib/libxcb
	$MAKE -C usr/src/external/mit/xorg/lib/libX11
	$MAKE -C usr/src/external/mit/xorg/lib/libXext
	$MAKE -C usr/src/external/mit/xorg/lib/libXv
	$MAKE -C usr/src/external/mit/xorg/lib/libXvMC
	cd usr/src/external/mit/xorg/lib
	build_and_install libI810XvMC
	build_and_install libIntelXvMC
	cd ../../../../../..
fi

# Create tar archive for X11R7 new files
tar -zcf $DISTDIR/usr-X11R7-drmgem.tgz \
    $(find /usr/X11R7 -newer .startbuild ! -type d)
