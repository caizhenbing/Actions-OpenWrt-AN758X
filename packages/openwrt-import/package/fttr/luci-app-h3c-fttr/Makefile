#
# LuCI application for the H3C HM2004-DU FMCS FTTR M-OLT FPGA.
#
# Following the zh-volt layout: the package directory is the feed entry, htdocs/
# lands on /www and root/ lands on /, so no install rules are needed here.
#
include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for the H3C FTTR master-OLT FPGA
LUCI_DEPENDS:=+luci-base +fttr-tools
LUCI_PKGARCH:=all

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=H3C HM2004-DU FTTR port

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
