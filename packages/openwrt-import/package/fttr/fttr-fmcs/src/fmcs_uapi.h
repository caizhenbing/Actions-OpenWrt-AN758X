/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Userspace ABI of the H3C FMCS (FTTR M-OLT) FPGA driver.
 *
 * This header is the contract between the kernel driver and userspace.  It is
 * a byte-for-byte reconstruction of the ABI of the vendor module
 * "fmcs.ko" (version V2024-0418-0001, vermagic 5.4.55 SMP mod_unload aarch64)
 * that shipped in the H3C HM2004-DU stock rootfs as
 * /lib/modules/fmcs.ko.
 *
 * Everything below was recovered by disassembling that module; the evidence
 * for each constant is recorded in docs/REVERSE_ENGINEERING.md.  Do not change
 * a value here without re-checking it against those notes: miniolt, moltd,
 * moltcmd, ploam and momci from the stock rootfs link against exactly these
 * numbers, and the whole point of this header is that those binaries keep
 * working unchanged.
 */
#ifndef _UAPI_FTTR_FMCS_H
#define _UAPI_FTTR_FMCS_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * The device node the vendor driver creates.  FmcsMciInit() calls
 * register_chrdev_region(MKDEV(0xE6, 0), 1, "fmcs_mci"), i.e. a fixed major of
 * 230 with minor 0; userspace opens "/dev/fmcs_mci".  The driver also
 * registers a sysfs class named "fmcs_mci".
 */
#define FMCS_MCI_DEV_NAME	"/dev/fmcs_mci"
#define FMCS_MCI_MAJOR		0xE6
#define FMCS_MCI_CLASS_NAME	"fmcs_mci"

/*
 * ioctl type byte.  FmcsMciIoctl() rejects any command whose bits 8..15 are
 * not 0xa5 before it even takes the ioctl mutex, so the type is load-bearing.
 */
#define FMCS_IOCTL_MAGIC	0xa5

/*
 * A 32-bit FPGA or BOSA register address paired with its 32-bit value.
 *
 * Written structs come straight from user memory, read structs are filled in
 * by the kernel and copied back, so the same eight bytes travel in both
 * directions.  FmcsMciIoctlReadReg() copies exactly 8 bytes in, calls the
 * read function with a pointer to the *second* word as the output slot, then
 * copies all 8 bytes back out -- hence a read ioctl is _IOWR even though the
 * vendor spelled the direction bit as _IOR for the ploam command below.
 */
struct fmcs_reg {
	__u32 addr;
	__u32 value;
};

/*
 * Ploam message length in bytes.  FmcsMciIoctlSendPloam() allocates exactly
 * 13 bytes (kmalloc-16) and copies 13 bytes from user space;
 * FmcsSpiRecvPloamMsg() reads exactly 13 bytes off the SPI bus and
 * miniolt parses its "send ploam" argument with thirteen "%02hhx"
 * conversions.  All three agree on 13.
 */
#define FMCS_PLOAM_LEN		13

/*
 * Placeholder type used only to reproduce the vendor's 8-byte size field on the
 * two PLOAM commands.
 *
 * Both commands carry an 8-byte _IOC size even though their payload is 13
 * bytes; the vendor declared them against an eight-byte type and never
 * validated _IOC_SIZE().  Keeping the wrong size is not cosmetic: the size
 * field is part of the command number, so `_IOW('a5', 4, __u8[13])` would
 * encode to 0x400da504 and no shipped binary would match it.  Encoding 8 is
 * what makes the constant 0x4008a504.
 */
struct fmcs_ioctl_size_quirk {
	/** @reserved: never transferred. */
	__u64 reserved;
};

/* Write one 32-bit FPGA register.  arg: struct fmcs_reg * */
#define FMCS_IOC_WR_FPGA_REG	_IOW(FMCS_IOCTL_MAGIC, 0, struct fmcs_reg)
/* Read one 32-bit FPGA register.   arg: struct fmcs_reg * (addr in, value out) */
#define FMCS_IOC_RD_FPGA_REG	_IOWR(FMCS_IOCTL_MAGIC, 1, struct fmcs_reg)
/* Write one 32-bit BOSA register.  arg: struct fmcs_reg * */
#define FMCS_IOC_WR_BOSA_REG	_IOW(FMCS_IOCTL_MAGIC, 2, struct fmcs_reg)
/* Read one 32-bit BOSA register.   arg: struct fmcs_reg * (addr in, value out) */
#define FMCS_IOC_RD_BOSA_REG	_IOWR(FMCS_IOCTL_MAGIC, 3, struct fmcs_reg)
/* Transmit a PLOAM message.        arg: __u8[FMCS_PLOAM_LEN] */
#define FMCS_IOC_SEND_PLOAM	_IOW(FMCS_IOCTL_MAGIC, 4, struct fmcs_ioctl_size_quirk)

/*
 * Pull one PLOAM message off the FPGA receive FIFO.
 *
 * The vendor handler ignores its argument completely and returns 0 whether or
 * not a message was found, so this ioctl is a trigger, not a transport: the
 * message itself is delivered to userspace through the genetlink family
 * "genl_PLOAM_NET" (see FMCS_GENL_* below).  Reproduced as-is because
 * moltcmd issues it and would otherwise block on a reply that never comes.
 */
#define FMCS_IOC_RECV_PLOAM	_IOR(FMCS_IOCTL_MAGIC, 5, struct fmcs_ioctl_size_quirk)

/*
 * Push N synthetic frames at the FTTR data path.
 *
 * The ioctl argument is *not* a pointer: FmcsMciIoctlSendHsgmiiPacket() takes
 * it as an unsigned 8-bit repetition count.  Each iteration allocates an
 * 0x80-byte skb, fills it with 0xa5, stamps a fixed 16-byte header from the
 * module's .rodata and queues it on the FTTR Ethernet netdev.  This is the
 * vendor's link bring-up / stress test and is only reachable from moltcmd's
 * debug verbs.
 */
#define FMCS_IOC_SEND_HSGMII	_IOW(FMCS_IOCTL_MAGIC, 6, __u8)

/*
 * ---------------------------------------------------------------------------
 * Genetlink families
 * ---------------------------------------------------------------------------
 *
 * FmcsNetlinkInit() registers seventeen families, all with version 1 and a
 * two-element operation table (a "sendto-usrs" notification op and an echo
 * op).  The names are read out of the module's .data at load time, so they are
 * part of the ABI for any userspace helper that joins them.
 *
 * Only the first is exported (EXPORT_SYMBOL(DRV_GENL_GenlSendToUsr)) and only
 * via __ksymtab; the rest are internal.  DRV_GENL_GenlSendToUsr() is how the
 * driver pushes asynchronous events -- PLOAM arrivals, LOS/LOB, ranging done,
 * OMCI traffic -- up to the userspace OMCI stack.
 */
#define FMCS_GENL_NAME_NET		"genl_NET"
#define FMCS_GENL_NAME_FPGA		"genl_FPGA"
#define FMCS_GENL_NAME_SPT_NET		"genl_SPT_NET"
#define FMCS_GENL_NAME_OTHER_NET	"genl_OTHER_NET"
#define FMCS_GENL_NAME_PLOAM_NET	"genl_PLOAM_NET"
#define FMCS_GENL_NAME_PLOAM_FPGA	"genl_PLOAM_FPGA"
#define FMCS_GENL_NAME_OMCI0_NET	"genl_OMCI0_NET"
#define FMCS_GENL_NAME_OMCI1_NET	"genl_OMCI1_NET"
#define FMCS_GENL_NAME_OMCI2_NET	"genl_OMCI2_NET"
#define FMCS_GENL_NAME_OMCI3_NET	"genl_OMCI3_NET"
#define FMCS_GENL_NAME_OMCI0_FPGA	"genl_OMCI0_FPGA"
#define FMCS_GENL_NAME_OMCI1_FPGA	"genl_OMCI1_FPGA"
#define FMCS_GENL_NAME_OMCI2_FPGA	"genl_OMCI2_FPGA"
#define FMCS_GENL_NAME_OMCI3_FPGA	"genl_OMCI3_FPGA"

#define FMCS_GENL_VERSION		1
#define FMCS_GENL_MCGRP_NAME		"fmcs"

/*
 * ---------------------------------------------------------------------------
 * Network devices
 * ---------------------------------------------------------------------------
 *
 * FmcsNetDeviceInit() brings up two ARPHRD_ETHER netdevs that the userspace
 * PLOAM and OMCI stacks bind to.  miniolt creates an AF_PACKET/SOCK_RAW
 * socket, resolves the interface index of "molt_omci" with SIOCGIFINDEX
 * (retrying ten times at two-second intervals) and SO_BINDTODEVICE-binds to
 * it.  The names are therefore ABI, not cosmetics.
 */
#define FMCS_NETDEV_PLOAM		"molt_ploam"
#define FMCS_NETDEV_OMCI		"molt_omci"

/*
 * The FTTR data path in front of the FPGA is a set of VLAN sub-interfaces of
 * the SoC Ethernet MAC.  FmcsFttrEthNetDevInit() resolves the master and up
 * to sixteen sub-interfaces *by name* at module load and fails the whole
 * module if any one of them is missing -- so the naming scheme is ABI too.
 */
#define FMCS_FTTR_ETH_MASTER		"eth1"
#define FMCS_FTTR_ETH_SUBFMT		"eth1.%u"
#define FMCS_FTTR_ETH_SUB_MAX		16

/*
 * ---------------------------------------------------------------------------
 * FPGA register addresses used by the vendor SPI layer
 * ---------------------------------------------------------------------------
 *
 * Only the addresses the driver itself touches while servicing events are
 * listed; the remainder of the register map is owned by userspace
 * (libminiolt_svc.so / moltd) and must not be guessed at here.
 */
#define FMCS_FPGA_REG_INT_STATUS0	0x0000
#define FMCS_FPGA_REG_INT_STATUS1	0x0004
#define FMCS_FPGA_REG_MCAST_GEMPORT	0x0008

/*
 * Value written into the GPIO interrupt-clear register by the threaded SPI
 * GPIO interrupt handler.  See docs/REVERSE_ENGINEERING.md: the handler
 * unconditionally writes 0xffffffff to (gpio_base + 0x08), clearing every
 * pending GPIO interrupt, then wakes the PLOAM receive thread.
 */
#define FMCS_GPIO_IRQ_CLEAR_ALL		0xffffffffu
#define FMCS_GPIO_IRQ_CLEAR_OFFSET	0x08

#endif /* _UAPI_FTTR_FMCS_H */
