dnl SPDX-License-Identifier: OSL-3.0
dnl Copyright (C) 2023  Trevor Woerner <twoerner@gmail.com>

AC_PREREQ(2.57)
AC_INIT([mqtt-gpio], [0.3.1], [twoerner@gmail.com], [mqtt-gpio])
AC_CONFIG_SRCDIR(src/mqtt-gpio.c)
AC_CONFIG_AUX_DIR(cfg)
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign no-dist-gzip dist-xz])
AM_CONFIG_HEADER(cfg/config.h)

SUBDIRS="src extras"

dnl **********************************
dnl checks for programs
dnl **********************************
AC_PROG_CC
AC_PROG_CPP
AC_PROG_MAKE_SET
AC_PROG_INSTALL
AC_PROG_LN_S

dnl **********************************
dnl checks for libraries
dnl **********************************
AC_SEARCH_LIBS(gpiod_chip_open_lookup,gpiod,,AC_MSG_ERROR([can't find gpiod library]),)
AC_SEARCH_LIBS(mosquitto_lib_init,mosquitto,,AC_MSG_ERROR([can't find mosquitto library]),)

dnl **********************************
dnl checks for header files
dnl **********************************
AC_HEADER_STDC
AC_CHECK_HEADERS(stdio.h string.h stdbool.h)
AC_CHECK_HEADERS(limits.h errno.h stdlib.h unistd.h getopt.h)
AC_CHECK_HEADERS(gpiod.h mosquitto.h)

dnl **********************************
dnl checks for typedefs, structs, and
dnl compiler characteristics
dnl **********************************
AC_TYPE_SIZE_T

dnl **********************************
dnl output
dnl **********************************
AC_SUBST(SUBDIRS)
AC_OUTPUT(Makefile
cfg/Makefile
extras/Makefile
src/Makefile)
