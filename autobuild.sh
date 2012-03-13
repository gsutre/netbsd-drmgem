#!/bin/sh
#
# Automated build script for netbsd-drmgem.  Needs a NetBSD-daily/HEAD time
# stamp, for instance:
#
# autobuild.sh 201106060220Z
#
# WARNING: this script modifies the local X11R7 installation!
#

# Cache this for message output
self="$(basename "$0")"

# Abort as soon as something wrong occurs
set -e

# URLS
NETBSD_DAILY_FTP_URL=ftp://nyftp.netbsd.org/pub/NetBSD-daily
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


# Main #########################################################################

# Check for NetBSD 6.99
set -- $(uname -r | tr '.' ' ')
if [ "$(uname -s)" != "NetBSD" -o "$1" != "6" -o "$2" != "99" ]; then
	error "host should be running NetBSD 6.99."
fi
set --

# Check remaining free space
set -- $(df -m . | tail -1)
if [ "$4" -lt "1500" ]; then
	error "insufficient free space ($4 MiB < 1500 MiB)."
fi
set --

# Check for wget
if ! which wget >/dev/null; then
	error "wget not found."
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

	# Download and extract source sets
	cat > src.files <<EOF
usr/src/build.sh
usr/src/etc
usr/src/external/mit
usr/src/include
usr/src/lib
usr/src/tools
EOF
	cat > syssrc.files <<EOF
usr/src/common
usr/src/sys
EOF
	cat > xsrc.files <<EOF
usr/xsrc/external/mit/MesaLib
usr/xsrc/external/mit/libXau
usr/xsrc/external/mit/libXdmcp
usr/xsrc/external/mit/libxcb
usr/xsrc/external/mit/libX11
usr/xsrc/external/mit/libXext
usr/xsrc/external/mit/libXv
usr/xsrc/external/mit/libXvMC
EOF
	sets="src syssrc xsrc"
	for set in $sets; do
		wget -O - ${NETBSD_DAILY_FTP_URL}/HEAD/$TIMESTAMP/source/sets/$set.tgz | \
		    tar -T $set.files -zxf -
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
