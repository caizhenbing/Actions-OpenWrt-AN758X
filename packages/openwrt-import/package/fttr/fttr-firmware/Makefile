#
# Pango Logos PGL50H configuration bitstream for the FMCS FPGA.
#
# The driver declares MODULE_FIRMWARE("FTTR_TOP.sbit") and loads it with
# request_firmware(), so the blob has to be in the rootfs at
# /lib/firmware/FTTR_TOP.sbit.  A board that already has the FPGA configured
# some other way can simply not install this.
#
# Put the bitstream at src/FTTR_TOP.sbit before building — the path is
# FTTR_FW_SRC_DIR below, and there is no fallback.  install-into-openwrt.sh
# copies it from the stock rootfs when it can find one.
#
# Whether to commit the 2 MB binary is the integrator's call; this repository
# does not ship it, but a build repo may (see docs/CI_INTEGRATION.md).  It is
# vendor-licensed (PKG_LICENSE below), not GPL like the driver.

include $(TOPDIR)/rules.mk

PKG_NAME:=fttr-firmware
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary
PKG_MAINTAINER:=H3C HM2004-DU FTTR port

FTTR_FW_SRC_DIR:=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))src)

include $(INCLUDE_DIR)/package.mk

define Package/fttr-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=FMCS FPGA bitstream (Pango Logos PGL50H)
  # The driver is a KernelPackage: OpenWrt names it kmod-fttr-fmcs, not
  # fttr-fmcs.  Depending on the bare name yields an unsatisfiable dependency.
  DEPENDS:=+kmod-fttr-fmcs
endef

define Package/fttr-firmware/description
  FTTR_TOP.sbit, the configuration bitstream for the FMCS downlink FPGA on the
  H3C HM2004-DU.

  The bitstream is fed to the FPGA configuration port byte for byte, MSB first,
  with no header handling: the stock loader does not parse it and neither does
  this port.  Its 141-byte header names the part as a Pango Logos
  PGL50H-6-FBG484.  See docs/SBIT_FORMAT.md.

  Licensed by the vendor; redistributing it is the integrator's call.
endef

# Nothing to compile: this package ships a file.
define Build/Compile
endef

define Package/fttr-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware
	$(INSTALL_DATA) $(FTTR_FW_SRC_DIR)/FTTR_TOP.sbit \
		$(1)/lib/firmware/FTTR_TOP.sbit
endef

$(eval $(call BuildPackage,fttr-firmware))
