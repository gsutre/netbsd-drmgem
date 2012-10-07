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

# Directories
DSTDIR=netbsd-drmgem.dist.$(uname -m)
USRDIR=netbsd-drmgem.usr


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
Usage: $self [options] <time stamp>

  -b <srt>    Use netbsd-drmgem branch <str> instead of master.
  -c          Continue build without downloading and patching sources.
  -j <int>    Specify the maximum number of make(1) jobs.
  -m          Also build the intel DRI modules.
  -v          Also build the intel XvMC libraries.

WARNING: this script modifies the local X11R7 installation!
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
	xsrc/external/mit/libX11 	\
	xsrc/external/mit/libXext 	\
	xsrc/external/mit/libXfixes 	\
	xsrc/external/mit/libXv 	\
	xsrc/external/mit/libXvMC 	\
	xsrc/external/mit/libxcb 	\
	xsrc/external/mit/xcb-util"

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

# Download and patch NetBSD sources
if [ "$continue" = "yes" ]; then
	echo "$self: continue build."

	if [ ! \( -e "netbsd-drmgem.sources_done" -a -d "$USRDIR" \) ]; then
		error "can't continue: missing file or directory."
	fi
else
	echo "$self: start build."

	# Remove leftover files (if any)
	rm -rf netbsd-drmgem netbsd-drmgem.* $DSTDIR $URSDIR

	# Time stamp for build start
	touch netbsd-drmgem.autobuild_start

	# Download and extract netbsd-drmgem
	wget --no-check-certificate -O - ${GITHUB_TARBALL_URL}/$BRANCH | tar -zxf -
	mv gsutre-netbsd-drmgem-* netbsd-drmgem
	rm -f pax_global_header

	# Get NetBSD sources by CVS
	for f in ${SRC_FILES} ${SYSSRC_FILES} ${XSRC_FILES}; do
		cvs -z3 export -N -d $USRDIR -D "$TIMESTAMP" "$f"
	done

	# Overwrite source files with netbsd-drmgem ones
	cp -av netbsd-drmgem/src netbsd-drmgem/xsrc $USRDIR

	touch netbsd-drmgem.sources_done
fi

export USETOOLS=no

# Create distribution directory for new kernel and X11R7 files
mkdir -p $DSTDIR

# Build and install new kernel
cd $USRDIR/src/sys/arch/$(uname -m)/conf
config GENERIC
cd ../compile/GENERIC
$MAKE depend
$MAKE
mv netbsd ../../../../../../../$DSTDIR/netbsd-GENERIC-drmgem
cd ../../../../../../..

# Build the math library
$MAKE -C $USRDIR/src/lib/libm

# Build and install OpenBSD Xenocara's libdrm and Xorg intel driver
cd $USRDIR/src/external/mit/xorg/lib
build_and_install libdrm
build_and_install libdrm_intel
cd ../server/drivers
build_and_install xf86-video-intel
cd ../../../../../../..

if [ "$mesa" = "yes" ]; then
	# Build the intel DRI modules
	$MAKE -C $USRDIR/src/external/mit/xorg/tools/glsl
	$MAKE -C $USRDIR/src/external/mit/expat
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/dri/libmesa
	cd $USRDIR/src/external/mit/xorg/lib/dri
	build_and_install i915
	build_and_install i965
	cd ../../../../../../..
fi

if [ "$xvmc" = "yes" ]; then
	# Build the intel XvMC libraries
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXau
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXdmcp
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libX11
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXext
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXfixes
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXv
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libXvMC
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/libxcb
	$MAKE -C $USRDIR/src/external/mit/xorg/lib/xcb-util
	cd $USRDIR/src/external/mit/xorg/lib
	build_and_install libI810XvMC
	build_and_install libIntelXvMC
	cd ../../../../../..
fi

# Create tar archive for X11R7 new files
tar -zcf $DSTDIR/usr-X11R7-drmgem.tgz \
    $(find /usr/X11R7 -newer netbsd-drmgem.autobuild_start ! -type d)

echo "$self: build complete."
echo "$self: resulting kernel: $DSTDIR/netbsd-GENERIC-drmgem"
echo "$self: new X11R7 files:  $DSTDIR/usr-X11R7-drmgem.tgz"
