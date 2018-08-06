test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

olddir=`pwd`
cd "$srcdir"

package=farstream
srcfile=farstream/fs-conference.c

# Make sure we have common
if test ! -f common/gst-autogen.sh;
then
  echo "+ Setting up common submodule"
  git submodule init
fi
git submodule update

# source helper functions
if test ! -f common/gst-autogen.sh;
then
  echo There is something wrong with your source tree.
  echo You are missing common/gst-autogen.sh
  exit 1
fi
. common/gst-autogen.sh

CONFIGURE_DEF_OPT='--enable-gtk-doc'

autogen_options $@

printf "+ check for build tools"
if test -z "$NOCHECK"; then
  echo

  printf "  checking for autoreconf ... "
  echo
  which "autoreconf" 2>/dev/null || {
    echo "not found! Please install the autoconf package."
    exit 1
  }

  printf "  checking for pkg-config ... "
  echo
  which "pkg-config" 2>/dev/null || {
    echo "not found! Please install pkg-config."
    exit 1
  }
else
  echo ": skipped version checks"
fi

# if no arguments specified then this will be printed
if test -z "$*" && test -z "$NOCONFIGURE"; then
  echo "+ checking for autogen.sh options"
  echo "  This autogen script will automatically run ./configure as:"
  echo "  ./configure $CONFIGURE_DEF_OPT"
  echo "  To pass any additional options, please specify them on the $0"
  echo "  command line."
fi

toplevel_check $srcfile

# aclocal
if test -f acinclude.m4; then rm acinclude.m4; fi

autoreconf --force --install || exit 1

test -n "$NOCONFIGURE" && {
  echo "+ skipping configure stage for package $package, as requested."
  echo "+ autogen.sh done."
  exit 0
}

cd "$olddir"

echo "+ running configure ... "
test ! -z "$CONFIGURE_DEF_OPT" && echo "  default flags:  $CONFIGURE_DEF_OPT"
test ! -z "$CONFIGURE_EXT_OPT" && echo "  external flags: $CONFIGURE_EXT_OPT"
echo

echo "$srcdir/configure" $CONFIGURE_DEF_OPT $CONFIGURE_EXT_OPT
"$srcdir/configure" $CONFIGURE_DEF_OPT $CONFIGURE_EXT_OPT || {
        echo "  configure failed"
        exit 1
}

echo "Now type 'make' to compile $package."
