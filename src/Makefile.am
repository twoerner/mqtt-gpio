## SPDX-License-Identifier: OSL-3.0
## Copyright (C) 2023  Trevor Woerner <twoerner@gmail.com>

########################
## src/Makefile.am
########################
SUBDIRS =
etcpkgdir = $(sysconfdir)/$(PACKAGE)
AM_CPPFLAGS = -Wall -Wextra -Werror -DETCPKGDIR=\"$(etcpkgdir)\"

bin_PROGRAMS = mqtt-gpio
mqtt_gpio_SOURCES = mqtt-gpio.c
