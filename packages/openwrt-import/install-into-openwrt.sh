#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Install the FMCS FTTR port into an OpenWrt source tree.
#
# Copies the three packages into package/fttr/, installs the FPGA bitstream if
# one is supplied, and applies the board device-tree change.  Safe to re-run:
# it reports what it overwrote and never removes anything it did not put there.
#
# This script lives in openwrt-import/ next to the packages it installs, so run
# it from wherever that folder is; it resolves its own location.
#
# Usage:
#   install-into-openwrt.sh [options] <openwrt-root>
#
# Options:
#   --firmware <FTTR_TOP.sbit>  install the bitstream into fttr-firmware/src/
#   --commit                    stage and commit the result with git
#   --no-dts                    skip the device-tree patch
#   -n, --dry-run               show what would happen, change nothing
#   -h, --help                  this text
#
# Examples:
#   ./install-into-openwrt.sh ~/ponwrt
#   ./install-into-openwrt.sh --firmware /mnt/mtd3/lib/firmware/FTTR_TOP.sbit ~/ponwrt
#   ./install-into-openwrt.sh --commit ~/ponwrt
#
# Manual alternative: copy package/fttr/ into the OpenWrt tree by hand and
# apply patches/0001-arm64-dts-airoha-hm2004-du-enable-fmcs.patch with
# git apply.  See README.md in this folder.

set -eu

PROG=$(basename "$0")
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

ROOT=
FIRMWARE=
COMMIT=no
DO_DTS=yes
DRY=no

die() { printf '%s: %s\n' "$PROG" "$1" >&2; exit 1; }
say() { printf '  %s\n' "$1"; }
step() { printf '\n== %s\n' "$1"; }

usage() { sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
	case $1 in
		--firmware) [ $# -ge 2 ] || die "--firmware needs a path"; FIRMWARE=$2; shift 2 ;;
		--commit)   COMMIT=yes; shift ;;
		--no-dts)   DO_DTS=no; shift ;;
		-n|--dry-run) DRY=yes; shift ;;
		-h|--help)  usage ;;
		-*)         die "unknown option $1" ;;
		*)          [ -z "$ROOT" ] || die "only one openwrt-root may be given"
		            ROOT=$1; shift ;;
	esac
done

[ -n "$ROOT" ] || die "missing <openwrt-root>; try --help"
[ -d "$ROOT" ] || die "no such directory: $ROOT"
ROOT=$(CDPATH= cd -- "$ROOT" && pwd)

# ---------------------------------------------------------------- preflight --
step "Checking $ROOT is an OpenWrt tree"

[ -f "$ROOT/rules.mk" ] || die "$ROOT does not look like an OpenWrt tree (no rules.mk)"
[ -d "$ROOT/target/linux/airoha" ] || die "$ROOT is not the airoha target; this port only supports it"
[ -d "$ROOT/include" ] || die "$ROOT has no include/ directory"
say "rules.mk, include/, target/linux/airoha/ all present"

SRC="$HERE/package/fttr"
[ -d "$SRC" ] || die "package sources not found at $SRC"
say "package sources at $SRC"

DTS_FILE="$ROOT/target/linux/airoha/dts/an7581-h3c-hm2004-du.dts"
PATCH="$HERE/patches/0001-arm64-dts-airoha-hm2004-du-enable-fmcs.patch"

# ------------------------------------------------------------------- copy ----
step "Installing packages into package/fttr/"

DST="$ROOT/package/fttr"
if [ "$DRY" = no ]; then
	mkdir -p "$DST"
	# cp -R of each package directory; contents are plain files.
	for d in "$SRC"/*; do
		[ -d "$d" ] || continue
		name=$(basename "$d")
		if [ -e "$DST/$name" ]; then
			say "overwriting existing package/fttr/$name"
		else
			say "package/fttr/$name"
		fi
		cp -R "$d" "$DST/"
	done
else
	for d in "$SRC"/*; do
		[ -d "$d" ] || continue
		say "would install package/fttr/$(basename "$d")"
	done
fi

# --------------------------------------------------------------- firmware ----
if [ -n "$FIRMWARE" ]; then
	step "Installing the bitstream"
	[ -f "$FIRMWARE" ] || die "no such firmware file: $FIRMWARE"
	size=$(wc -c < "$FIRMWARE" | tr -d ' ')
	# The driver feeds the file verbatim; a truncated copy would still load and
	# still be wrong, so check the size the stock image ships.
	if [ "$size" != "2087000" ]; then
		say "WARNING: $FIRMWARE is $size bytes; FTTR_TOP.sbit is 2087000"
		say "         a different bitstream is fine, a truncated one is not"
	fi
	if [ "$DRY" = no ]; then
		mkdir -p "$DST/fttr-firmware/src"
		cp "$FIRMWARE" "$DST/fttr-firmware/src/FTTR_TOP.sbit"
		say "package/fttr/fttr-firmware/src/FTTR_TOP.sbit ($size bytes)"
	else
		say "would install fttr-firmware/src/FTTR_TOP.sbit ($size bytes)"
	fi
else
	step "Bitstream not supplied"
	say "fttr-firmware will fail to build until src/FTTR_TOP.sbit exists."
	say "Re-run with --firmware <path>, or drop the file in by hand:"
	say "  $DST/fttr-firmware/src/FTTR_TOP.sbit"
fi

# -------------------------------------------------------------------- dts ----
if [ "$DO_DTS" = yes ]; then
	step "Applying the device-tree change"
	if [ ! -f "$DTS_FILE" ]; then
		say "SKIP: $DTS_FILE not found"
	elif grep -q 'nconfig-gpios' "$DTS_FILE" 2>/dev/null; then
		say "already applied (nconfig-gpios present); leaving it alone"
	elif [ "$DRY" = yes ]; then
		say "would apply $(basename "$PATCH")"
	else
		applied=no
		if command -v git >/dev/null 2>&1 && \
		   git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
			if git -C "$ROOT" apply --check "$PATCH" 2>/dev/null; then
				git -C "$ROOT" apply "$PATCH"
				say "applied with git apply"
				applied=yes
			fi
		fi
		if [ "$applied" = no ] && command -v patch >/dev/null 2>&1; then
			if patch -p1 -d "$ROOT" --dry-run -s -i "$PATCH" 2>/dev/null; then
				patch -p1 -d "$ROOT" -s -i "$PATCH"
				say "applied with patch"
				applied=yes
			fi
		fi
		[ "$applied" = yes ] || die "could not apply $(basename "$PATCH"); apply it by hand and re-run with --no-dts"
	fi
else
	step "Skipping the device-tree change (--no-dts)"
fi

# ----------------------------------------------------------------- commit ----
if [ "$COMMIT" = yes ] && [ "$DRY" = no ]; then
	step "Committing"
	if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
		git -C "$ROOT" add package/fttr target/linux/airoha/dts/an7581-h3c-hm2004-du.dts
		git -C "$ROOT" commit -q -m "airoha: add FMCS FTTR master-OLT support for the H3C HM2004-DU

Adds the fttr-fmcs driver, the fttr-tools userspace utility and the
luci-app-h3c-fttr page, plus the device-tree node for the FMCS FPGA.

The driver publishes /dev/fmcs_mci with the vendor ioctl ABI, the
molt_ploam and molt_omci control net devices, and the genl_* event
families, so the stock miniolt and OMCI stack keep working unchanged.

Register access still needs generic SPI transfers on the controller at
0x1fa10000, which the SPI-NAND driver currently owns; the bitstream
loader works independently of that."
		say "committed"
	else
		say "not a git repository; skipping commit"
	fi
fi

# ------------------------------------------------------------------- next ----
step "Next"
say "select the packages:"
say "  make menuconfig"
say "    Kernel modules -> Other modules -> fttr-fmcs"
say "    Firmware        -> fttr-firmware"
say "    Utilities       -> fttr-tools"
say "    LuCI -> Applications -> luci-app-h3c-fttr"
say "then: make -j\$(nproc)"
printf '\n%s: done\n' "$PROG"
