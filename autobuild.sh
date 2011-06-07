#!/bin/sh
#
# Automatic build script for netbsd-drmgem.  Needs a NetBSD-daily/HEAD time
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
NETBSD_DAILY_FTP_URL=ftp://ftp.fr.netbsd.org/pub/NetBSD-daily
GITHUB_TARBALL_URL=https://github.com/gsutre/netbsd-drmgem/tarball

# Use netbsd-drmgem's master branch by default (selectable by option)
BRANCH=master


# Command-line Processing ######################################################

usage ()
{
	cat <<EOF
Usage: $self [-b <branch>] <time stamp>
EOF
}

while getopts "b:" option; do
	case $option in
		b)
			BRANCH="$OPTARG"
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

# Check for NetBSD 5.99
set -- $(uname -r | tr '.' ' ')
if [ "$(uname -s)" != "NetBSD" -o "$1" != "5" -o "$2" != "99" ]; then
	echo "$self: error: host should be running NetBSD 5.99."
	exit 1
fi
set --

# Check remaining free space
set -- $(df -m . | tail -1)
if [ "$4" -lt "1500" ]; then
	echo "$self: error: insufficient free space ($4 MiB < 1500 MiB)."
	exit 1
fi
set --

# Check for wget
if ! which wget >/dev/null; then
	echo "$self: error: wget not found."
	exit 1
fi

# Check for sudo
if ! which sudo >/dev/null; then
	echo "$self: error: sudo not found."
	exit 1
fi

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

# Create distribution directory
DISTDIR=dist.$(uname -m)
mkdir -p $DISTDIR

# Build and install new kernel
cd usr/src/sys/arch/$(uname -m)/conf
config GENERIC
cd ../compile/GENERIC
make depend
make
mv netbsd ../../../../../../../$DISTDIR/netbsd-GENERIC-drmgem
cd ../../../../../../..

# Build and install OpenBSD Xenocara's libdrm and Xorg intel driver
touch start-X-build
export USETOOLS=no
cd usr/src/external/mit/xorg/lib/libdrm
(sudo -E make includes) && make && (sudo -E make install)
cd ../libdrm_intel
(sudo -E make includes) && make && (sudo -E make install)
cd ../../server/drivers/xf86-video-intel
(sudo -E make includes) && make && (sudo -E make install)
cd ../../../../../../../..

# Build the intel DRI modules
make -C usr/src/lib/libm
make -C usr/src/external/mit/xorg/tools/glsl
make -C usr/src/external/mit/xorg/lib/expat
make -C usr/src/external/mit/xorg/lib/dri/libmesa
cd usr/src/external/mit/xorg/lib/dri/i915
(sudo -E make includes) && make && (sudo -E make install)
cd ../i965
(sudo -E make includes) && make && (sudo -E make install)
cd ../../../../../../../..

# Create tar archive for X11R7 new files
tar -zcf $DISTDIR/usr-X11R7-drmgem.tgz \
    $(find /usr/X11R7 -newer start-X-build ! -type d)
