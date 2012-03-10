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


# Command-line Processing ######################################################

usage ()
{
	cat <<EOF
Usage: $self [-cmv] [-b <branch>] [-j <njobs>] <time stamp>
EOF
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
			exit 1
			;;
	esac
done

shift $(($OPTIND - 1))

TIMESTAMP="$1"

if [ -z "$TIMESTAMP" ]; then
	usage
	exit 1
fi


# Main #########################################################################

# Check for NetBSD 6.99
set -- $(uname -r | tr '.' ' ')
if [ "$(uname -s)" != "NetBSD" -o "$1" != "6" -o "$2" != "99" ]; then
	echo "$self: error: host should be running NetBSD 6.99." 1>&2
	exit 1
fi
set --

# Check remaining free space
set -- $(df -m . | tail -1)
if [ "$4" -lt "1500" ]; then
	echo "$self: error: insufficient free space ($4 MiB < 1500 MiB)." 1>&2
	exit 1
fi
set --

# Check for wget
if ! which wget >/dev/null; then
	echo "$self: error: wget not found." 1>&2
	exit 1
fi

# Check for sudo
if ! which sudo >/dev/null; then
	echo "$self: error: sudo not found." 1>&2
	exit 1
fi

DISTDIR=dist.$(uname -m)

if [ "$continue" = "yes" ]; then
	if [ ! \( -d "$DISTDIR" -a -e ".startbuild" -a -d "usr" \) ]; then
		echo "$self: can't continue: missing file or directory." 1>&2
		exit 1
	fi
else
	# Create distribution directory and time stamp for build start
	mkdir -p $DISTDIR
	touch .startbuild

	# Download and extract netbsd-drmgem
	wget --no-check-certificate -O - ${GITHUB_TARBALL_URL}/$BRANCH | tar -zxf -
	mv gsutre-netbsd-drmgem-* netbsd-drmgem

	# Download and extract source sets
	cat > excludes <<EOF
CVS
usr/xsrc/xfree
EOF
	sets="src syssrc xsrc"
	for set in $sets; do
		wget -O - ${NETBSD_DAILY_FTP_URL}/HEAD/$TIMESTAMP/source/sets/$set.tgz | \
		    tar -X excludes -zxf -
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
cd usr/src/external/mit/xorg/lib/libdrm
(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
cd ../libdrm_intel
(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
cd ../../server/drivers/xf86-video-intel
(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
cd ../../../../../../../..

if [ "$mesa" = "yes" ]; then
	# Build the intel DRI modules
	$MAKE -C usr/src/external/mit/xorg/tools/glsl
	$MAKE -C usr/src/external/mit/expat
	$MAKE -C usr/src/external/mit/xorg/lib/dri/libmesa
	cd usr/src/external/mit/xorg/lib/dri/i915
	(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
	cd ../i965
	(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
	cd ../../../../../../../..
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
	cd usr/src/external/mit/xorg/lib/libI810XvMC
	(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
	cd ../libIntelXvMC
	(sudo -E $MAKE includes) && $MAKE && (sudo -E $MAKE install)
	cd ../../../../../../..
fi

# Create tar archive for X11R7 new files
tar -zcf $DISTDIR/usr-X11R7-drmgem.tgz \
    $(find /usr/X11R7 -newer .startbuild ! -type d)
