// SPDX-License-Identifier: GPL-2.0-only
/*
 * H3C FMCS (FTTR M-OLT) FPGA driver - configuration bitstream loader.
 *
 * Replaces the vendor module fpga_load.ko.  That module bit-bangs the FPGA
 * configuration port from hard-coded GPIO numbers, reads
 * /lib/firmware/FTTR_TOP.sbit through filp_open()/kernel_read() and is
 * triggered by writing to /proc/fpgaLoad; see docs/REVERSE_ENGINEERING.md
 * section 6 for the disassembly evidence.
 *
 * This port keeps the sequence and the signal map exactly, but:
 *
 *   - takes the GPIO lines from the device tree as named descriptors instead
 *     of poking the ECNT GPIO block at raw offsets, so it works on the
 *     mainline pinctrl driver;
 *   - loads the bitstream with request_firmware() rather than filp_open();
 *   - keeps a /proc entry with the same command grammar, because the vendor's
 *     /proc interface is the only way to retune the clock hold times on a
 *     running board and the bring-up notes depend on it.
 *
 * The bitstream is fed to the configuration port byte for byte, MSB first,
 * with no header handling: the vendor loader does not parse the file and
 * FTTR_TOP.sbit must be presented exactly as shipped.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "fmcs.h"

#define FMCS_FIRMWARE_NAME	"FTTR_TOP.sbit"

/*
 * Vendor hold times, in nanoseconds, for the configuration clock.
 *
 * fpga_load.ko declares holdtime2 = 0 and holdtime1 = 3 (the BSS zero-init only
 * covers holdtime2; holdtime1 is stored explicitly during module init), and the
 * /proc write handler refuses to lower holdtime1 below 3.  Reprogramming a
 * slower FPGA part may need larger values; both stay runtime tunable.
 */
static unsigned int holdtime1 = 3;
static unsigned int holdtime2;

module_param(holdtime1, uint, 0644);
MODULE_PARM_DESC(holdtime1, "Configuration clock setup/high hold time in ns");
module_param(holdtime2, uint, 0644);
MODULE_PARM_DESC(holdtime2, "Configuration clock low hold time in ns");

/**
 * struct fmcs_bitstream - configuration port lines.
 *
 * The vendor drives nCONFIG, RST, CLK and DATA and samples two status inputs.
 * Every line is optional at the device-tree level; a board that configures the
 * FPGA some other way simply does not describe them and the loader stays idle.
 */
struct fmcs_bitstream {
	/** @fmcs: owning driver instance. */
	struct fmcs_dev *fmcs;
	/** @nconfig: program request, active low. */
	struct gpio_desc *nconfig;
	/** @rst: FPGA reset, released once configuration completes. */
	struct gpio_desc *rst;
	/** @clk: configuration clock. */
	struct gpio_desc *clk;
	/** @data: serial configuration data. */
	struct gpio_desc *data;
	/** @status: nSTATUS-equivalent input, sampled before the transfer. */
	struct gpio_desc *status;
	/** @conf_done: configuration-done input, sampled afterwards. */
	struct gpio_desc *conf_done;
	/** @proc: /proc/fpgaLoad entry. */
	struct proc_dir_entry *proc;
	/** @lock: serialises loads triggered from probe and /proc. */
	struct mutex lock;
};

static struct fmcs_bitstream *fmcs_bs;

/* ------------------------------------------------------------------------ */
/* GPIO clocking                                                            */
/* ------------------------------------------------------------------------ */

/**
 * fmcs_write_firm_to_fpga() - shift a bitstream out of the configuration port.
 * @bs: configuration port lines.
 * @buf: bitstream bytes.
 * @len: number of bytes in @buf.
 *
 * Each byte goes out MSB first.  DATA is set, then CLK is pulsed high and
 * again low, with the hold times applied either side.  The vendor's timing is
 * reproduced exactly: holdtime1 before the rising edge and after it, holdtime2
 * before the next data bit.  cond_resched() runs once per byte so that a
 * two-megabyte bitstream does not stall the CPU for the whole transfer.
 */
static void fmcs_write_firm_to_fpga(struct fmcs_bitstream *bs, const u8 *buf,
				    size_t len)
{
	const u8 *end = buf + len;
	const u8 *p;

	for (p = buf; p != end; p++) {
		int bit;

		for (bit = 7; bit >= 0; bit--) {
			gpiod_set_value_cansleep(bs->data, (*p >> bit) & 1);
			ndelay(holdtime1);

			gpiod_set_value_cansleep(bs->clk, 1);
			ndelay(holdtime1);

			gpiod_set_value_cansleep(bs->clk, 0);
			ndelay(holdtime2);
		}

		cond_resched();
	}
}

/* ------------------------------------------------------------------------ */
/* Load sequence                                                            */
/* ------------------------------------------------------------------------ */

/* Vendor delays, in microseconds; see section 3.7 of the reverse notes. */
#define FMCS_LOAD_NCONFIG_LOW_US	1
#define FMCS_LOAD_NCONFIG_HIGH_US	5000
#define FMCS_LOAD_PRE_STREAM_US		300
#define FMCS_LOAD_POST_DONE_US		1000

int fmcs_bitstream_load(struct fmcs_dev *fmcs)
{
	struct fmcs_bitstream *bs = fmcs_bs;
	const struct firmware *fw;
	int ret;

	if (!bs || !bs->nconfig || !bs->clk || !bs->data) {
		dev_dbg(fmcs->dev, "no configuration port described, not loading\n");
		return -ENODEV;
	}

	ret = request_firmware(&fw, FMCS_FIRMWARE_NAME, fmcs->dev);
	if (ret) {
		dev_err(fmcs->dev, "failed to load %s: %d\n",
			FMCS_FIRMWARE_NAME, ret);
		return ret;
	}

	mutex_lock(&bs->lock);

	dev_info(fmcs->dev, "Fpga load start......\n");

	/* Pulse nCONFIG to make the FPGA latch the bitstream from scratch. */
	gpiod_set_value_cansleep(bs->nconfig, 0);
	fsleep(FMCS_LOAD_NCONFIG_LOW_US);
	gpiod_set_value_cansleep(bs->nconfig, 1);
	fsleep(FMCS_LOAD_NCONFIG_HIGH_US);

	/*
	 * A clear status line here means the FPGA never came out of reset.  The
	 * vendor only logs this and continues, so a board whose status line is
	 * not wired still loads; keep that behaviour.
	 */
	if (bs->status && !gpiod_get_value_cansleep(bs->status))
		dev_warn(fmcs->dev, "Check fpga state flag fail.\n");

	fsleep(FMCS_LOAD_PRE_STREAM_US);

	fmcs_write_firm_to_fpga(bs, fw->data, fw->size);

	if (!bs->conf_done || gpiod_get_value_cansleep(bs->conf_done)) {
		fsleep(FMCS_LOAD_POST_DONE_US);
		if (bs->rst)
			gpiod_set_value_cansleep(bs->rst, 1);
		dev_info(fmcs->dev, "Fpga load and rst success.\n");
		ret = 0;
	} else {
		dev_err(fmcs->dev, "Fpga load and rst fail.\n");
		ret = -EIO;
	}

	mutex_unlock(&bs->lock);

	release_firmware(fw);

	return ret;
}

/* ------------------------------------------------------------------------ */
/* /proc/fpgaLoad                                                           */
/* ------------------------------------------------------------------------ */

static int fpga_load_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Fpga load info: holdtime1 = %u, holdtime2 = %u.\n",
		   holdtime1, holdtime2);

	return 0;
}

static int fpga_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, fpga_load_show, NULL);
}

/**
 * fpga_load_write() - handle `load <t1> <t2> <ignored>` on /proc/fpgaLoad.
 * @file: proc file.
 * @ubuf: user buffer.
 * @count: bytes in @ubuf.
 * @ppos: file position.
 *
 * Mirrors the vendor grammar: the format string is "%31s %u %u %31s", only the
 * first token is inspected, and the first numeric argument is applied only when
 * it is greater than 2.  A load is then retriggered.
 *
 * Returns @count on success, or a negative errno.
 */
static ssize_t fpga_load_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char kernel_buf[128];
	char cmd[32] = { 0 };
	char tail[32] = { 0 };
	unsigned int t1 = 0, t2 = 0;
	int n;

	if (count == 0 || count >= sizeof(kernel_buf))
		return -EINVAL;

	if (copy_from_user(kernel_buf, ubuf, count))
		return -EFAULT;
	kernel_buf[count] = '\0';

	n = sscanf(kernel_buf, "%31s %u %u %31s", cmd, &t1, &t2, tail);
	if (n < 1 || strcmp(cmd, "load") != 0) {
		pr_info(FMCS_DRV_NAME ": wrong fpga load cmd\n");
		return count;
	}

	if (t1 > 2)
		holdtime1 = t1;
	holdtime2 = t2;

	pr_info(FMCS_DRV_NAME ": Set hold time success, t1 = %u, t2 = %u.\n",
		holdtime1, holdtime2);

	if (fmcs_bs)
		fmcs_bitstream_load(fmcs_bs->fmcs);

	return count;
}

static const struct proc_ops fpga_load_proc_ops = {
	.proc_open	= fpga_load_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= fpga_load_write,
};

/* ------------------------------------------------------------------------ */
/* Initialisation                                                           */
/* ------------------------------------------------------------------------ */

static int fmcs_bitstream_get_gpio(struct device *dev, const char *name,
				   enum gpiod_flags flags,
				   struct gpio_desc **out)
{
	struct gpio_desc *desc = devm_gpiod_get_optional(dev, name, flags);

	if (IS_ERR(desc))
		return dev_err_probe(dev, PTR_ERR(desc),
				     "failed to get %s gpio\n", name);

	*out = desc;

	return 0;
}

int fmcs_bitstream_init(struct fmcs_dev *fmcs)
{
	struct fmcs_bitstream *bs;
	struct device *dev = fmcs->dev;
	int ret;

	bs = devm_kzalloc(dev, sizeof(*bs), GFP_KERNEL);
	if (!bs)
		return -ENOMEM;

	bs->fmcs = fmcs;
	mutex_init(&bs->lock);

	ret = fmcs_bitstream_get_gpio(dev, "nconfig", GPIOD_OUT_HIGH, &bs->nconfig);
	if (ret)
		return ret;
	ret = fmcs_bitstream_get_gpio(dev, "rst", GPIOD_OUT_LOW, &bs->rst);
	if (ret)
		return ret;
	ret = fmcs_bitstream_get_gpio(dev, "clk", GPIOD_OUT_LOW, &bs->clk);
	if (ret)
		return ret;
	ret = fmcs_bitstream_get_gpio(dev, "data", GPIOD_OUT_LOW, &bs->data);
	if (ret)
		return ret;
	ret = fmcs_bitstream_get_gpio(dev, "status", GPIOD_IN, &bs->status);
	if (ret)
		return ret;
	ret = fmcs_bitstream_get_gpio(dev, "conf-done", GPIOD_IN, &bs->conf_done);
	if (ret)
		return ret;

	if (!bs->nconfig && !bs->clk && !bs->data) {
		dev_info(dev, "no FPGA configuration port described\n");
		return 0;
	}

	/* /proc/fpgaLoad is 0666 in the vendor module and scripts depend on it. */
	bs->proc = proc_create("fpgaLoad", 0666, NULL, &fpga_load_proc_ops);
	if (!bs->proc) {
		dev_err(dev, "Failed to create proc entry\n");
		return -EIO;
	}

	fmcs_bs = bs;

	return 0;
}

void fmcs_bitstream_exit(struct fmcs_dev *fmcs)
{
	struct fmcs_bitstream *bs = fmcs_bs;

	if (!bs)
		return;

	fmcs_bs = NULL;

	if (bs->proc)
		remove_proc_entry("fpgaLoad", NULL);
}
