/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * H3C FMCS (FTTR M-OLT) FPGA driver - internal definitions.
 *
 * Port of the vendor module fmcs.ko (V2024-0418-0001) from the H3C HM2004-DU
 * stock firmware (Linux 5.4.55, aarch64) to mainline/OpenWrt Linux 6.18.
 * See docs/REVERSE_ENGINEERING.md for the evidence behind every constant.
 */
#ifndef _FMCS_H
#define _FMCS_H

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "fmcs_uapi.h"

#define FMCS_DRV_NAME		"fmcs"
#define FMCS_DRV_VERSION	"V2024-0418-0001"

/*
 * SPI opcodes.  The vendor SPI core takes a single command byte followed by a
 * big-endian address and, for writes, a big-endian value; results come back
 * either as four big-endian bytes (FPGA registers) or one byte (BOSA).
 */
#define FMCS_SPI_OP_WRITE	0xA8
#define FMCS_SPI_OP_RDCMD	0xA0
#define FMCS_SPI_OP_RDDATA	0x50
#define FMCS_SPI_OP_PLOAM_TX	0xAA
#define FMCS_SPI_OP_PLOAM_RX	0xA2
#define FMCS_SPI_OP_PLOAM_RD	0x52

/* Second byte of the PLOAM receive command; the remaining three are zero. */
#define FMCS_SPI_PLOAM_RX_ARG	0x10

/*
 * Inter-phase delays.  The vendor passes these as literal __const_udelay()
 * loop counts; see docs/REVERSE_ENGINEERING.md section 3.7 for why the
 * microsecond values below are approximations of those literals.
 */
#define FMCS_DELAY_FPGA_US	50
#define FMCS_DELAY_BOSA_US	600

/* Largest frame the driver ever clocks out: PLOAM transmit is 5 + 13. */
#define FMCS_SPI_FRAME_MAX	18

struct fmcs_dev;

/*
 * Register transport.
 *
 * The vendor driver reaches the FPGA through the EcoNet SDK's spi_device_*
 * calls on the SoC SPI controller at 0x1fa10000.  That controller is claimed
 * by the SPI-NAND driver in the 6.18 tree and exposes no generic transfers, so
 * the port keeps the wire protocol in one place and offers two back ends: a
 * standard SPI device (used as soon as a generic master exists for that bus)
 * and a GPIO bit-bang implementation, which works on the stock wiring today.
 */
struct fmcs_transport {
	/** @dev: owning device, for devm allocation and diagnostics. */
	struct device *dev;
	/** @spi: SPI slave to use, or NULL when bit-banging. */
	struct spi_device *spi;
	/**
	 * @tx: GPIOs for the bit-bang back end, or NULL when @spi is set.
	 * Either all four are present or none are.
	 */
	struct gpio_desc *sck;
	/** @mosi: bit-bang data-out line. */
	struct gpio_desc *mosi;
	/** @miso: bit-bang data-in line. */
	struct gpio_desc *miso;
	/** @cs: bit-bang chip-select line, active low. */
	struct gpio_desc *cs;
	/** @delay_ns: half-period for the bit-bang clock. */
	u32 delay_ns;
	/** @buf: scratch frame buffer, avoiding a stack frame per transfer. */
	u8 buf[FMCS_SPI_FRAME_MAX];
	/** @lock: serialises transfers; held across a read's write/read pair. */
	struct mutex lock;
};

/**
 * struct fmcs_dev - driver instance state.
 *
 * One instance per "h3c,fmcs" device tree node.  The vendor module used file
 * scope globals because it supported exactly one FPGA; the port keeps the same
 * single-instance ABI (a fixed device node and fixed netdev names) but stores
 * state on the instance so the code reads as a driver rather than a script.
 */
struct fmcs_dev {
	/** @dev: platform device backing this instance. */
	struct device *dev;
	/** @transport: register transport. */
	struct fmcs_transport transport;

	/** @irq: threaded interrupt number. */
	int irq;
	/** @irq_gpio: the line the interrupt is gated through, if any. */
	struct gpio_desc *irq_gpio;

	/** @reset_gpio: FPGA reset line, if one is described. */
	struct gpio_desc *reset_gpio;

	/* ---- character device ---- */
	/** @cdev: character device for /dev/fmcs_mci. */
	struct cdev cdev;
	/** @devt: dev_t the character device is registered under. */
	dev_t devt;
	/** @class: sysfs class the node is published under. */
	struct class *class;
	/** @device: sysfs device for the node. */
	struct device *chardev;
	/** @ioctl_lock: serialises register access from the ioctl path. */
	struct mutex ioctl_lock;

	/* ---- net devices ---- */
	/** @ploam_netdev: the molt_ploam net device. */
	struct net_device *ploam_netdev;
	/** @omci_netdev: the molt_omci net device. */
	struct net_device *omci_netdev;

	/* ---- event handling ---- */
	/** @event_task: thread draining FPGA interrupt status registers. */
	struct task_struct *event_task;
	/** @event_wait: wait queue the interrupt handler wakes. */
	wait_queue_head_t event_wait;
	/** @event_stop: set to ask @event_task to exit. */
	bool event_stop;
	/** @irq_count: interrupts observed, for diagnostics. */
	atomic_t irq_count;
};

/* ---- fmcs_spi.c: register transport ---- */

int fmcs_transport_init(struct fmcs_dev *fmcs);
void fmcs_transport_exit(struct fmcs_dev *fmcs);

int fmcs_spi_write_fpga_reg(struct fmcs_dev *fmcs, u32 addr, u32 value);
int fmcs_spi_read_fpga_reg(struct fmcs_dev *fmcs, u32 addr, u32 *value);
int fmcs_spi_write_bosa_reg(struct fmcs_dev *fmcs, u32 addr, u32 value);
int fmcs_spi_read_bosa_reg(struct fmcs_dev *fmcs, u32 addr, u32 *value);
int fmcs_spi_send_ploam(struct fmcs_dev *fmcs, const u8 ploam[FMCS_PLOAM_LEN]);
int fmcs_spi_recv_ploam(struct fmcs_dev *fmcs, u8 ploam[FMCS_PLOAM_LEN]);

/* ---- fmcs_load.c: bitstream loading ---- */

int fmcs_bitstream_init(struct fmcs_dev *fmcs);
void fmcs_bitstream_exit(struct fmcs_dev *fmcs);
/**
 * fmcs_bitstream_load() - load a bitstream into the FPGA configuration port.
 * @fmcs: driver instance.
 *
 * Returns 0 when the FPGA reports configuration done, a negative errno
 * otherwise.  Safe to call again to reconfigure the device.
 */
int fmcs_bitstream_load(struct fmcs_dev *fmcs);

/* ---- fmcs.c: shared helpers ---- */

/** @fmcs_log_level: mirrors the vendor uiLogLevel module parameter. */
extern unsigned int fmcs_log_level;

#endif /* _FMCS_H */
