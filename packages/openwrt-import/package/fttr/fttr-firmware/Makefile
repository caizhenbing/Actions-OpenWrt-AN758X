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

# 必须在任何 include 之前固定本包目录。OpenWrt 的 rules.mk/package.mk 包含链末尾
# 不是本 Makefile，否则 $(lastword $(MAKEFILE_LIST)) 会指向别的 .mk（如工具链下的片段），
# 导致 FTTR_FW_SRC_DIR 被解析成 $(TOOLCHAIN_DIR)/src，install 阶段找不到 sbit。
FTTR_FW_PKG_DIR:=$(abspath $(dir $(lastword $(MAKEFILE_LIST))))
FTTR_FW_SRC_DIR:=$(FTTR_FW_PKG_DIR)/src

include $(TOPDIR)/rules.mk

PKG_NAME:=fttr-firmware
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary
PKG_MAINTAINER:=H3C HM2004-DU FTTR port

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
