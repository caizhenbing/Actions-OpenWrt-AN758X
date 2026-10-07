// SPDX-License-Identifier: GPL-2.0-only
/*
 * H3C FMCS (FTTR M-OLT) FPGA driver - register transport.
 *
 * Implements the SPI wire protocol recovered from the vendor module fmcs.ko.
 * Every frame below is reproduced byte for byte; see
 * docs/REVERSE_ENGINEERING.md sections 3.1 to 3.6 for the disassembly evidence.
 *
 * The vendor reaches the FPGA through the EcoNet SDK's spi_device_write() and
 * spi_device_read() helpers.  In the 6.18 tree the controller those helpers
 * drive (0x1fa10000) is owned by the SPI-NAND driver and offers no generic
 * transfers, so this file keeps the protocol in one place and provides two back
 * ends behind it:
 *
 *   - a standard SPI device, used when the device tree gives the fmcs node an
 *     "spi" child and a generic master is bound to the bus;
 *   - a GPIO bit-bang implementation, used when the device tree describes sck,
 *     mosi, miso and cs lines instead.
 *
 * Both back ends produce identical bytes on the wire.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#include "fmcs.h"

/*
 * The vendor delays the read phase with __const_udelay() using its own literals
 * tuned for the 5.4 build's loops_per_jiffy.  Those literals do not carry over
 * to a different kernel, so the port uses the microsecond equivalents derived
 * in docs/REVERSE_ENGINEERING.md section 3.7.  Both are far longer than the
 * FPGA needs; they exist to let the slave's FIFO drain before the data phase.
 */
static void fmcs_spi_read_delay(u32 usec)
{
	fsleep(usec);
}

/* ------------------------------------------------------------------------ */
/* Raw byte-level back ends                                                 */
/* ------------------------------------------------------------------------ */

/**
 * fmcs_gpio_xfer() - clock bytes out of and into the FPGA over GPIO bit-bang.
 * @fmcs: driver instance.
 * @tx: bytes to transmit; may be NULL when @tx_len is 0.
 * @tx_len: number of bytes in @tx.
 * @rx: buffer for received bytes; may be NULL when @rx_len is 0.
 * @rx_len: number of bytes to receive.
 *
 * Mode 0 (clock idle low, sample on the leading edge), MSB first.  The vendor
 * hardware controller uses the same mode; nothing in the recovered protocol
 * depends on a different clocking.
 *
 * Returns 0 on success, -EINVAL if the transport has no bit-bang lines.
 */
static int fmcs_gpio_xfer(struct fmcs_dev *fmcs, const u8 *tx, size_t tx_len,
			  u8 *rx, size_t rx_len)
{
	struct fmcs_transport *t = &fmcs->transport;
	size_t total = tx_len + rx_len;
	size_t i;

	if (!t->sck || !t->mosi || !t->miso || !t->cs)
		return -EINVAL;

	gpiod_set_value_cansleep(t->cs, 1);

	for (i = 0; i < total; i++) {
		bool tx_phase = i < tx_len;
		u8 out = tx_phase ? tx[i] : 0xff;
		u8 in = 0;
		int bit;

		for (bit = 7; bit >= 0; bit--) {
			gpiod_set_value_cansleep(t->mosi, (out >> bit) & 1);
			ndelay(t->delay_ns);

			gpiod_set_value_cansleep(t->sck, 1);
			ndelay(t->delay_ns);

			in = (in << 1) | (gpiod_get_value_cansleep(t->miso) ? 1 : 0);

			gpiod_set_value_cansleep(t->sck, 0);
			ndelay(t->delay_ns);
		}

		if (!tx_phase)
			rx[i - tx_len] = in;
	}

	gpiod_set_value_cansleep(t->cs, 0);

	return 0;
}

/**
 * fmcs_spi_raw_write() - transmit a frame, with no data phase.
 * @fmcs: driver instance.
 * @len: number of valid bytes in the transport's scratch buffer.
 *
 * Returns 0 on success, or a negative errno.
 */
static int fmcs_spi_raw_write(struct fmcs_dev *fmcs, size_t len)
{
	struct fmcs_transport *t = &fmcs->transport;
	int ret;

	if (t->spi)
		return spi_write(t->spi, t->buf, len);

	ret = fmcs_gpio_xfer(fmcs, t->buf, len, NULL, 0);
	if (ret)
		return ret;

	return 0;
}

/**
 * fmcs_spi_raw_read() - clock out one command byte and read the response.
 * @fmcs: driver instance.
 * @cmd: command byte, written to buf[0] before the transfer.
 * @rx: buffer of @rx_len bytes for the response.
 * @rx_len: number of bytes to receive.
 *
 * The vendor calls this as spi_device_read(buf, 1, rx, rx_len) with a single
 * command byte in buf[0].  On a mainline SPI device the equivalent is
 * spi_write_then_read(), which transmits the one byte before clocking the
 * response in.
 *
 * Returns 0 on success, or a negative errno.
 */
static int fmcs_spi_raw_read(struct fmcs_dev *fmcs, u8 cmd, u8 *rx, size_t rx_len)
{
	struct fmcs_transport *t = &fmcs->transport;

	t->buf[0] = cmd;

	if (t->spi)
		return spi_write_then_read(t->spi, t->buf, 1, rx, rx_len);

	return fmcs_gpio_xfer(fmcs, t->buf, 1, rx, rx_len);
}

/* ------------------------------------------------------------------------ */
/* Protocol layer                                                           */
/* ------------------------------------------------------------------------ */

/**
 * fmcs_reg_write() - write a 32-bit register on either address window.
 * @fmcs: driver instance.
 * @addr: register address.
 * @value: value to write.
 *
 * Frame: @FMCS_SPI_OP_WRITE, address big-endian, value big-endian - nine bytes
 * total.  The FPGA and BOSA windows share this opcode; only the address space
 * differs, exactly as in the vendor module.
 *
 * Returns 0 on success, or a negative errno.
 */
static int fmcs_reg_write(struct fmcs_dev *fmcs, u32 addr, u32 value)
{
	struct fmcs_transport *t = &fmcs->transport;
	__be32 be_addr = cpu_to_be32(addr);
	__be32 be_value = cpu_to_be32(value);
	int ret;

	mutex_lock(&t->lock);
	t->buf[0] = FMCS_SPI_OP_WRITE;
	memcpy(&t->buf[1], &be_addr, sizeof(be_addr));
	memcpy(&t->buf[5], &be_value, sizeof(be_value));
	ret = fmcs_spi_raw_write(fmcs, 9);
	mutex_unlock(&t->lock);

	return ret;
}

/**
 * fmcs_reg_read() - read a register, returning the raw four response bytes.
 * @fmcs: driver instance.
 * @addr: register address.
 * @rx: four-byte response buffer.
 *
 * Two-phase: the read command is issued first, then the slave is given time to
 * fetch the value before the data phase clocks it out.
 *
 * Returns 0 on success, or a negative errno.
 */
static int fmcs_reg_read(struct fmcs_dev *fmcs, u32 addr, u8 rx[4], u32 delay_us)
{
	struct fmcs_transport *t = &fmcs->transport;
	__be32 be_addr = cpu_to_be32(addr);
	int ret;

	/* Held across both phases: another transfer must not interleave. */
	mutex_lock(&t->lock);

	memset(t->buf, 0, sizeof(t->buf));
	t->buf[0] = FMCS_SPI_OP_RDCMD;
	memcpy(&t->buf[1], &be_addr, sizeof(be_addr));
	ret = fmcs_spi_raw_write(fmcs, 5);
	if (ret)
		goto out;

	fmcs_spi_read_delay(delay_us);

	ret = fmcs_spi_raw_read(fmcs, FMCS_SPI_OP_RDDATA, rx, 4);

out:
	mutex_unlock(&t->lock);

	return ret;
}

int fmcs_spi_write_fpga_reg(struct fmcs_dev *fmcs, u32 addr, u32 value)
{
	return fmcs_reg_write(fmcs, addr, value);
}

int fmcs_spi_read_fpga_reg(struct fmcs_dev *fmcs, u32 addr, u32 *value)
{
	u8 rx[4];
	int ret;

	ret = fmcs_reg_read(fmcs, addr, rx, FMCS_DELAY_FPGA_US);
	if (ret)
		return ret;

	*value = ((u32)rx[0] << 24) | ((u32)rx[1] << 16) |
		 ((u32)rx[2] << 8) | (u32)rx[3];

	return 0;
}

int fmcs_spi_write_bosa_reg(struct fmcs_dev *fmcs, u32 addr, u32 value)
{
	return fmcs_reg_write(fmcs, addr, value);
}

int fmcs_spi_read_bosa_reg(struct fmcs_dev *fmcs, u32 addr, u32 *value)
{
	u8 rx[4];
	int ret;

	ret = fmcs_reg_read(fmcs, addr, rx, FMCS_DELAY_BOSA_US);
	if (ret)
		return ret;

	/*
	 * A BOSA register is one byte wide and arrives in the last of the four
	 * response bytes; the vendor returns only that byte.
	 */
	*value = rx[3];

	return 0;
}

int fmcs_spi_send_ploam(struct fmcs_dev *fmcs, const u8 ploam[FMCS_PLOAM_LEN])
{
	struct fmcs_transport *t = &fmcs->transport;
	int ret;

	/*
	 * Frame: opcode, three zero bytes, then the 13-byte payload - 18 bytes.
	 * The vendor builds this with two overlapping 64-bit stores; a plain
	 * copy produces the same bytes.
	 */
	mutex_lock(&t->lock);
	memset(t->buf, 0, sizeof(t->buf));
	t->buf[0] = FMCS_SPI_OP_PLOAM_TX;
	memcpy(&t->buf[5], ploam, FMCS_PLOAM_LEN);
	ret = fmcs_spi_raw_write(fmcs, 5 + FMCS_PLOAM_LEN);
	mutex_unlock(&t->lock);

	return ret;
}

int fmcs_spi_recv_ploam(struct fmcs_dev *fmcs, u8 ploam[FMCS_PLOAM_LEN])
{
	struct fmcs_transport *t = &fmcs->transport;
	u8 rx[FMCS_PLOAM_LEN];
	int ret;

	mutex_lock(&t->lock);

	memset(t->buf, 0, sizeof(t->buf));
	t->buf[0] = FMCS_SPI_OP_PLOAM_RX;
	t->buf[1] = FMCS_SPI_PLOAM_RX_ARG;
	ret = fmcs_spi_raw_write(fmcs, 5);
	if (ret)
		goto out;

	fmcs_spi_read_delay(FMCS_DELAY_FPGA_US);

	ret = fmcs_spi_raw_read(fmcs, FMCS_SPI_OP_PLOAM_RD, rx, sizeof(rx));
	if (ret)
		goto out;

	memcpy(ploam, rx, sizeof(rx));

out:
	mutex_unlock(&t->lock);

	return ret;
}

/* ------------------------------------------------------------------------ */
/* Initialisation                                                           */
/* ------------------------------------------------------------------------ */

static int fmcs_transport_gpio_init(struct fmcs_dev *fmcs)
{
	struct fmcs_transport *t = &fmcs->transport;
	u32 hz = 1000000;

	t->sck = devm_gpiod_get_optional(t->dev, "sck", GPIOD_OUT_LOW);
	t->mosi = devm_gpiod_get_optional(t->dev, "mosi", GPIOD_OUT_LOW);
	t->miso = devm_gpiod_get_optional(t->dev, "miso", GPIOD_IN);
	t->cs = devm_gpiod_get_optional(t->dev, "cs", GPIOD_OUT_LOW);

	if (IS_ERR(t->sck))
		return dev_err_probe(t->dev, PTR_ERR(t->sck), "failed to get sck\n");
	if (IS_ERR(t->mosi))
		return dev_err_probe(t->dev, PTR_ERR(t->mosi), "failed to get mosi\n");
	if (IS_ERR(t->miso))
		return dev_err_probe(t->dev, PTR_ERR(t->miso), "failed to get miso\n");
	if (IS_ERR(t->cs))
		return dev_err_probe(t->dev, PTR_ERR(t->cs), "failed to get cs\n");

	if (!t->sck)
		return -ENODEV;

	if (!t->mosi || !t->miso || !t->cs)
		return dev_err_probe(t->dev, -EINVAL,
				     "bit-bang transport needs sck, mosi, miso and cs\n");

	device_property_read_u32(t->dev, "spi-max-frequency", &hz);
	if (!hz)
		hz = 1000000;

	/* One nanosecond of half-period per megahertz keeps this an integer. */
	t->delay_ns = DIV_ROUND_UP(500000, hz);
	if (!t->delay_ns)
		t->delay_ns = 1;

	dev_info(t->dev, "using GPIO bit-bang transport at %u Hz\n", hz);

	return 0;
}

int fmcs_transport_init(struct fmcs_dev *fmcs)
{
	struct fmcs_transport *t = &fmcs->transport;
	int ret;

	t->dev = fmcs->dev;
	mutex_init(&t->lock);

	/*
	 * The caller sets t->spi when the device tree binds this driver as a
	 * SPI client, i.e. when the fmcs node is a child of the controller.
	 * That is the preferred wiring; the GPIO bit-bang path below needs no
	 * generic master and is what works on a stock HM2004-DU today, where
	 * the controller is owned by the SPI-NAND driver.
	 */
	if (t->spi) {
		dev_info(t->dev, "using hardware SPI on %s\n",
			 dev_name(&t->spi->dev));
		return 0;
	}

	ret = fmcs_transport_gpio_init(fmcs);
	if (ret)
		return ret;

	return 0;
}

void fmcs_transport_exit(struct fmcs_dev *fmcs)
{
	struct fmcs_transport *t = &fmcs->transport;
	int i;

	/* Leave the bus idle so a reload sees a clean clock. */
	if (t->sck) {
		gpiod_set_value_cansleep(t->sck, 0);
		gpiod_set_value_cansleep(t->cs, 0);
	}

	for (i = 0; i < ARRAY_SIZE(t->buf); i++)
		t->buf[i] = 0;
}
