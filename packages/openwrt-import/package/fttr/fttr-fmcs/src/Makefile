# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree module build for the FMCS FTTR driver.
#
# This is driver/Makefile with the Kconfig conditional removed.  An OpenWrt
# KernelPackage builds with M=$(PKG_BUILD_DIR) and never reads the kernel's
# Kconfig, so an obj-$(CONFIG_...) line here would expand to an empty object
# list and the build would silently produce no module.  The CONFIG symbols live
# in the package's KCONFIG= instead; see docs/PACKAGING.md section 3.
#
# CONFIG_FTTR_FMCS_LOAD_AT_PROBE does *not* arrive through autoconf.h: it is
# in no Kconfig file, so kconfig drops it from the kernel .config before
# autoconf.h is generated.  The package Makefile passes it with CFLAGS_MODULE
# instead; see the comment on Build/Compile there.  Do not add an
# obj-$(CONFIG_FTTR_FMCS) line here either — that would expand empty under M=.

obj-m				+= fttr-fmcs.o

fttr-fmcs-y			:= fmcs.o fmcs_spi.o fmcs_load.o
