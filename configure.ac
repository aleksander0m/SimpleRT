dnl Process this file with autoconf to produce a configure script.
AC_PREREQ([2.68])

AC_INIT([g-simple-rt], [0.0.2], [aleksander@aleksander.es])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 no-define no-dist-gzip dist-xz tar-ustar -Wno-portability foreign])
AM_MAINTAINER_MODE([enable])

dnl Support silent build rules. Disable
dnl by either passing --disable-silent-rules to configure or passing V=1
dnl to make
AM_SILENT_RULES([yes])

dnl Required programs
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL

dnl Initialize libtool
LT_PREREQ([2.2])
LT_INIT

dnl Required dependency versions
GLIB_VERSION=2.36
GUDEV_VERSION=147
LIBUSB_VERSION=1.0

dnl GLib, GIO...
PKG_CHECK_MODULES(GLIB,
                  glib-2.0 >= $GLIB_VERSION
                  gobject-2.0
                  gio-2.0
                  gio-unix-2.0)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(GUDEV, [gudev-1.0 >= $GUDEV_VERSION])
AC_SUBST(GUDEV_CFLAGS)
AC_SUBST(GUDEV_LIBS)

PKG_CHECK_MODULES(LIBUSB, [libusb-1.0 >= $LIBUSB_VERSION])
AC_SUBST(LIBUSB_CFLAGS)
AC_SUBST(LIBUSB_LIBS)

AC_CONFIG_FILES([
    Makefile
    simple-rt-cli/Makefile
])

AC_OUTPUT

echo "
    g-simple-rt $VERSION
    ==============================================

    compiler:        ${CC}
    cflags:          ${CFLAGS}
    maintainer mode: ${USE_MAINTAINER_MODE}
"
