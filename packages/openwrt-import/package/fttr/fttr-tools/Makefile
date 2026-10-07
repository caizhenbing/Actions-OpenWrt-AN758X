#
# miniolt - command line access to the FMCS FTTR M-OLT FPGA registers
#
# Userspace package.  Sources live in src/ beside this file, including its own
# copy of fmcs_uapi.h; that header is the userspace ABI and must stay identical
# to the one in fttr-fmcs/src/.

include $(TOPDIR)/rules.mk

PKG_NAME:=fttr-tools
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=H3C HM2004-DU FTTR port

FTTR_TOOLS_SRC_DIR:=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))src)

include $(INCLUDE_DIR)/package.mk

define Package/fttr-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=miniolt - FMCS FTTR M-OLT register and PLOAM tool
  DEPENDS:=+fttr-fmcs
endef

define Package/fttr-tools/description
  Source port of the stock miniolt utility.

  Reads and writes FPGA and BOSA registers through /dev/fmcs_mci using the
  vendor ioctl numbers verbatim, so it is interchangeable with the binary
  shipped in the stock rootfs, and injects PLOAM and OMCI frames on the
  molt_omci net device.

  Register access requires the fttr-fmcs register transport to be working; see
  that package's description.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(FTTR_TOOLS_SRC_DIR)/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CPPFLAGS="-I$(PKG_BUILD_DIR)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra"
endef

define Package/fttr-tools/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/miniolt $(1)/usr/bin/miniolt
endef

$(eval $(call BuildPackage,fttr-tools))
