#
# H3C FMCS (FTTR M-OLT) FPGA driver
#
# Kernel module package.  The driver sources live in src/ beside this file, so
# the package is self-contained and needs no PKG_SOURCE_URL.
#
# Builds out of tree with M=$(PKG_BUILD_DIR); src/Makefile is the module
# Makefile.  The CONFIG symbols this build needs are declared in KCONFIG below
# rather than in a Kconfig file, because an out-of-tree M= build never reads the
# kernel's Kconfig.  See docs/PACKAGING.md section 3.

# 必须在任何 include 之前用 $(lastword $(MAKEFILE_LIST)) 固定本包目录。
# kmod 的 Build/Prepare 在内核 kbuild 上下文执行时，MAKEFILE_LIST 末尾会变成
# 内核 Makefile，那时再取就会解析成 $(LINUX_DIR)/src（该目录不存在），导致 cp 失败。
FTTR_FMCS_PKG_DIR:=$(abspath $(dir $(lastword $(MAKEFILE_LIST))))
FTTR_FMCS_SRC_DIR:=$(FTTR_FMCS_PKG_DIR)/src

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=fttr-fmcs
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=H3C HM2004-DU FTTR port

# Sources ship beside this Makefile (FTTR_FMCS_SRC_DIR 已在文件头固定), nothing to download.

include $(INCLUDE_DIR)/package.mk

define KernelPackage/fttr-fmcs
  SUBMENU:=Other modules
  TITLE:=H3C FMCS FTTR M-OLT FPGA support
  DEPENDS:=@TARGET_airoha
  KCONFIG:= \
	CONFIG_FTTR_FMCS=m \
	CONFIG_FTTR_FMCS_LOAD_AT_PROBE=y
  FILES:=$(PKG_BUILD_DIR)/fttr-fmcs.ko
  AUTOLOAD:=$(call AutoProbe,fttr-fmcs)
endef

define KernelPackage/fttr-fmcs/description
  Driver for the FMCS downlink FPGA that provides the FTTR master-OLT function
  on the H3C HM2004-DU.

  Publishes /dev/fmcs_mci with the vendor ioctl ABI that the stock miniolt,
  moltd and moltcmd expect, the molt_ploam and molt_omci control net devices,
  and the genl_* event families the stock PLOAM and OMCI stacks use.

  Two parts work independently:

    * the bitstream loader, which drives the FPGA configuration port from the
      GPIOs named in the device tree and needs no SPI controller support;
    * the register transport, which needs the SPI controller at 0x1fa10000 to
      serve generic transfers.  Until it does, register reads and writes fail,
      and that failure is reported rather than hidden.

  Requires the device tree node added by
  patches/0001-arm64-dts-airoha-hm2004-du-enable-fmcs.patch.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(FTTR_FMCS_SRC_DIR)/* $(PKG_BUILD_DIR)/
endef

# CONFIG_FTTR_FMCS_LOAD_AT_PROBE is declared in KCONFIG above, which puts it
# in the kernel .config — but it exists in no Kconfig file, so kconfig drops it
# (scripts/kconfig/confdata.c: sym_find() fails, "unknown symbol") and it never
# reaches include/generated/autoconf.h, which is what a module built with M=
# actually sees.  The #ifdef in fmcs.c would therefore always be false and the
# bitstream load would be compiled out silently.  kernel.mk does
# `-include $(LINUX_DIR)/.config`, so the value IS visible here; pass it down
# explicitly.
FTTR_LOAD_AT_PROBE:=$(if $(filter y,$(CONFIG_FTTR_FMCS_LOAD_AT_PROBE)),1,0)

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(KERNEL_MAKE_FLAGS) \
		M="$(PKG_BUILD_DIR)" \
		CFLAGS_MODULE="-DCONFIG_FTTR_FMCS_LOAD_AT_PROBE=$(FTTR_LOAD_AT_PROBE)" \
		modules
endef

$(eval $(call KernelPackage,fttr-fmcs))
