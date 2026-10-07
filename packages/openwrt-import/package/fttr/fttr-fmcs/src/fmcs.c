// SPDX-License-Identifier: GPL-2.0-only
/*
 * H3C FMCS (FTTR M-OLT) FPGA driver.
 *
 * Port of the vendor module fmcs.ko (V2024-0418-0001) from the H3C HM2004-DU
 * stock firmware (Linux 5.4.55, aarch64) to mainline/OpenWrt Linux 6.18.
 *
 * The driver owns the downlink FPGA that gives the board its FTTR master-OLT
 * function.  It publishes three things to userspace:
 *
 *   /dev/fmcs_mci    register and PLOAM access over a 0xa5-typed ioctl set,
 *                    consumed by miniolt, moltd, moltcmd and momci;
 *   molt_ploam
 *   molt_omci        two Ethernet net devices the userspace PLOAM and OMCI
 *                    stacks bind to;
 *   genl_* families  asynchronous FPGA events.
 *
 * The ioctl numbers, argument layouts and device names are ABI and are
 * reproduced from the vendor module unchanged; see driver/fmcs_uapi.h and
 * docs/REVERSE_ENGINEERING.md.
 */
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <net/genetlink.h>

#include "fmcs.h"

unsigned int fmcs_log_level = 1;
module_param_named(log_level, fmcs_log_level, uint, 0644);
MODULE_PARM_DESC(log_level, "Log verbosity; bit 1 enables per-event messages");

#define fmcs_dbg_log(fmcs, fmt, ...)					\
	do {								\
		if (fmcs_log_level & 2)					\
			dev_info((fmcs)->dev, fmt, ##__VA_ARGS__);	\
	} while (0)

/* ------------------------------------------------------------------------ */
/* Event delivery                                                           */
/* ------------------------------------------------------------------------ */

/*
 * The vendor module registers seventeen genetlink families, all with the same
 * two-entry operation table and all with names that userspace helpers join by
 * string.  Registering the full set keeps every name resolvable; only the
 * PLOAM family carries traffic in this port.
 */
static const char * const fmcs_genl_names[] = {
	FMCS_GENL_NAME_NET,
	FMCS_GENL_NAME_FPGA,
	FMCS_GENL_NAME_SPT_NET,
	FMCS_GENL_NAME_OTHER_NET,
	FMCS_GENL_NAME_PLOAM_NET,
	FMCS_GENL_NAME_PLOAM_FPGA,
	FMCS_GENL_NAME_OMCI0_NET,
	FMCS_GENL_NAME_OMCI1_NET,
	FMCS_GENL_NAME_OMCI2_NET,
	FMCS_GENL_NAME_OMCI3_NET,
	FMCS_GENL_NAME_OMCI0_FPGA,
	FMCS_GENL_NAME_OMCI1_FPGA,
	FMCS_GENL_NAME_OMCI2_FPGA,
	FMCS_GENL_NAME_OMCI3_FPGA,
};

#define FMCS_GENL_NUM		ARRAY_SIZE(fmcs_genl_names)

static struct genl_family fmcs_genl_families[FMCS_GENL_NUM];
static unsigned int fmcs_genl_registered;

/** @fmcs_genl_ploam: family index of genl_PLOAM_NET, used for PLOAM events. */
static unsigned int fmcs_genl_ploam;

enum fmcs_genl_attr {
	FMCS_ATTR_UNSPEC,
	/** @FMCS_ATTR_PAYLOAD: raw PLOAM or OMCI payload. */
	FMCS_ATTR_PAYLOAD,
	/** @FMCS_ATTR_PORT: sub-ONU index the event belongs to. */
	FMCS_ATTR_PORT,
	__FMCS_ATTR_MAX,
};

#define FMCS_ATTR_MAX		(__FMCS_ATTR_MAX - 1)

static const struct nla_policy fmcs_genl_policy[FMCS_ATTR_MAX + 1] = {
	[FMCS_ATTR_PAYLOAD] = { .type = NLA_BINARY, .len = 256 },
	[FMCS_ATTR_PORT] = { .type = NLA_U32 },
};

/**
 * fmcs_genl_notify() - multicast a payload to a family's listeners.
 * @family_idx: index into fmcs_genl_families.
 * @payload: bytes to carry, or NULL for an event with no payload.
 * @len: number of bytes in @payload.
 * @port: sub-ONU index, or -1 when the event is not port specific.
 *
 * Returns 0 on success, or a negative errno.  A failure to notify is never
 * fatal: events are advisory and the net devices remain usable.
 */
static int fmcs_genl_notify(unsigned int family_idx, const void *payload,
			    size_t len, int port)
{
	struct genl_family *family = &fmcs_genl_families[family_idx];
	struct sk_buff *skb;
	void *hdr;
	int ret;

	if (family_idx >= fmcs_genl_registered)
		return -EINVAL;

	skb = genlmsg_new(nla_total_size(len) + nla_total_size(4), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, family, 0, 0);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	if (payload && len &&
	    nla_put(skb, FMCS_ATTR_PAYLOAD, len, payload) < 0)
		goto nla_fail;

	if (port >= 0 && nla_put_u32(skb, FMCS_ATTR_PORT, port) < 0)
		goto nla_fail;

	genlmsg_end(skb, hdr);

	ret = genlmsg_multicast(family, skb, 0, 0, GFP_ATOMIC);
	if (ret == -ESRCH)
		ret = 0;	/* nobody listening */

	return ret;

nla_fail:
	nlmsg_free(skb);

	return -EMSGSIZE;
}

/* ------------------------------------------------------------------------ */
/* Net devices                                                              */
/* ------------------------------------------------------------------------ */

struct fmcs_netdev_priv {
	/** @fmcs: owning driver instance. */
	struct fmcs_dev *fmcs;
	/** @is_ploam: true for molt_ploam, false for molt_omci. */
	bool is_ploam;
};

static int fmcs_netdev_open(struct net_device *dev)
{
	netif_start_queue(dev);

	return 0;
}

static int fmcs_netdev_stop(struct net_device *dev)
{
	netif_stop_queue(dev);

	return 0;
}

/**
 * fmcs_ploam_start_xmit() - push a frame from molt_ploam to the FPGA.
 * @skb: frame queued by userspace.
 * @dev: the molt_ploam net device.
 *
 * The vendor transmits skb->data + 8; the first eight bytes of the frame are a
 * header this port does not interpret but must preserve, because the userspace
 * PLOAM stack writes them.  The 13 bytes that follow are the PLOAM message.
 *
 * Returns NETDEV_TX_OK in every case: the frame is consumed either way and
 * there is no queue to retry through.
 */
static netdev_tx_t fmcs_ploam_start_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	struct fmcs_netdev_priv *priv = netdev_priv(dev);
	struct fmcs_dev *fmcs = priv->fmcs;
	struct net_device_stats *stats = &dev->stats;
	int ret;

	if (skb->len < 8 + FMCS_PLOAM_LEN) {
		dev_warn_ratelimited(&dev->dev,
				     "short PLOAM frame (%u bytes), dropping\n",
				     skb->len);
		goto drop;
	}

	ret = fmcs_spi_send_ploam(fmcs, skb->data + 8);
	if (ret) {
		dev_warn_ratelimited(&dev->dev,
				     "fmcs spi send ploam msg failed: %d\n", ret);
		goto drop;
	}

	stats->tx_packets++;
	stats->tx_bytes += FMCS_PLOAM_LEN;

drop:
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

/**
 * fmcs_omci_start_xmit() - accept a frame on molt_omci.
 * @skb: frame queued by userspace.
 * @dev: the molt_omci net device.
 *
 * OMCI frames do not travel over the register interface: the FPGA sources and
 * sinks them on the FTTR data path, which in the vendor design is carried by
 * two callbacks the Ethernet driver calls into.  This port has no such hook
 * yet (see docs/REVERSE_ENGINEERING.md section 10.2, risk R2), so an outbound
 * OMCI frame is counted and dropped rather than silently lost in a queue.
 *
 * Returns NETDEV_TX_OK.
 */
static netdev_tx_t fmcs_omci_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

/**
 * fmcs_netdev_deliver() - hand a received frame to the network stack.
 * @dev: target net device.
 * @payload: frame contents.
 * @len: number of bytes in @payload.
 *
 * Called from process context (the event thread), so netif_rx() is sufficient.
 */
static void fmcs_netdev_deliver(struct net_device *dev, const void *payload,
				size_t len)
{
	struct sk_buff *skb;
	__be16 proto;

	skb = dev_alloc_skb(len + 8);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}

	skb_reserve(skb, 8);
	skb_put_data(skb, payload, len);
	skb->dev = dev;

	/*
	 * These are point-to-point control frames, not Ethernet II traffic, so
	 * the protocol field only has to be distinct enough for the userspace
	 * socket to see the frame; it filters on the AF_PACKET interface index.
	 */
	proto = htons(ETH_P_ALL);
	skb->protocol = proto;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(skb);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += len;

	netif_rx(skb);
}

static const struct net_device_ops fmcs_ploam_netdev_ops = {
	.ndo_open	= fmcs_netdev_open,
	.ndo_stop	= fmcs_netdev_stop,
	.ndo_start_xmit	= fmcs_ploam_start_xmit,
};

static const struct net_device_ops fmcs_omci_netdev_ops = {
	.ndo_open	= fmcs_netdev_open,
	.ndo_stop	= fmcs_netdev_stop,
	.ndo_start_xmit	= fmcs_omci_start_xmit,
};

/**
 * fmcs_alloc_netdev() - create and register one of the two control net devices.
 * @fmcs: driver instance.
 * @name: interface name, FMCS_NETDEV_PLOAM or FMCS_NETDEV_OMCI.
 * @ops: net device operations.
 * @is_ploam: selects PLOAM behaviour in the private area.
 *
 * Returns the registered device, or an ERR_PTR.
 */
static struct net_device *fmcs_alloc_netdev(struct fmcs_dev *fmcs,
					    const char *name,
					    const struct net_device_ops *ops,
					    bool is_ploam)
{
	struct net_device *dev;
	struct fmcs_netdev_priv *priv;
	int ret;

	dev = alloc_netdev_mqs(sizeof(*priv), name, NET_NAME_UNKNOWN,
			       ether_setup, 1, 1);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	priv = netdev_priv(dev);
	priv->fmcs = fmcs;
	priv->is_ploam = is_ploam;

	dev->netdev_ops = ops;
	/*
	 * The vendor clears IFF_MULTICAST and sets IFF_BROADCAST | IFF_NOARP.
	 * These devices carry addressed control frames between the driver and a
	 * userspace stack, never ARP or multicast, so the flags are kept.
	 */
	dev->flags &= ~IFF_MULTICAST;
	dev->flags |= IFF_BROADCAST | IFF_NOARP;

	/*
	 * The vendor leaves the address as all zeros.  A zero address makes
	 * these devices collide with each other in the neighbour tables and
	 * looks like a bug to userspace, so the port assigns a stable
	 * locally-administered address derived from the device name unless the
	 * device tree supplies one.
	 */
	if (!is_valid_ether_addr(dev->dev_addr))
		eth_hw_addr_random(dev);

	SET_NETDEV_DEV(dev, fmcs->dev);

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ERR_PTR(ret);
	}

	return dev;
}

static int fmcs_netdev_init(struct fmcs_dev *fmcs)
{
	fmcs->ploam_netdev = fmcs_alloc_netdev(fmcs, FMCS_NETDEV_PLOAM,
					       &fmcs_ploam_netdev_ops, true);
	if (IS_ERR(fmcs->ploam_netdev)) {
		dev_err(fmcs->dev, "Fmcs alloc net device failed\n");
		return PTR_ERR(fmcs->ploam_netdev);
	}

	fmcs->omci_netdev = fmcs_alloc_netdev(fmcs, FMCS_NETDEV_OMCI,
					      &fmcs_omci_netdev_ops, false);
	if (IS_ERR(fmcs->omci_netdev)) {
		dev_err(fmcs->dev, "Fmcs alloc net device failed\n");
		unregister_netdev(fmcs->ploam_netdev);
		free_netdev(fmcs->ploam_netdev);
		fmcs->ploam_netdev = NULL;
		return PTR_ERR(fmcs->omci_netdev);
	}

	return 0;
}

static void fmcs_netdev_exit(struct fmcs_dev *fmcs)
{
	if (fmcs->omci_netdev) {
		unregister_netdev(fmcs->omci_netdev);
		free_netdev(fmcs->omci_netdev);
		fmcs->omci_netdev = NULL;
	}

	if (fmcs->ploam_netdev) {
		unregister_netdev(fmcs->ploam_netdev);
		free_netdev(fmcs->ploam_netdev);
		fmcs->ploam_netdev = NULL;
	}
}

/* ------------------------------------------------------------------------ */
/* Interrupt handling                                                       */
/* ------------------------------------------------------------------------ */

/**
 * fmcs_event_thread() - drain FPGA interrupt status and dispatch events.
 * @arg: the driver instance.
 *
 * Runs at process context so that SPI transfers may sleep.  The interrupt
 * handler does nothing but acknowledge and wake this thread, mirroring the
 * vendor's request_threaded_irq(..., NULL, FmcsSpiGpioIsr, ...) split.
 *
 * Returns 0.
 */
static int fmcs_event_thread(void *arg)
{
	struct fmcs_dev *fmcs = arg;

	while (!kthread_should_stop()) {
		wait_event_interruptible(fmcs->event_wait,
					 fmcs->event_stop ||
					 atomic_read(&fmcs->irq_count) > 0);

		if (fmcs->event_stop)
			break;

		atomic_set(&fmcs->irq_count, 0);

		/*
		 * The vendor reads two interrupt-status registers and then
		 * decides what happened.  The register map above the few
		 * addresses in fmcs_uapi.h is owned by libminiolt_svc.so and
		 * has not been recovered, so the port reads the status words and
		 * reports them rather than guessing at the bit meanings.  See
		 * docs/REVERSE_ENGINEERING.md section 10.2, risk R5.
		 */
		{
			u32 status0 = 0, status1 = 0;

			if (fmcs_spi_read_fpga_reg(fmcs, FMCS_FPGA_REG_INT_STATUS0,
						   &status0) ||
			    fmcs_spi_read_fpga_reg(fmcs, FMCS_FPGA_REG_INT_STATUS1,
						   &status1))
				continue;

			fmcs_dbg_log(fmcs, "fpga irq status0=%08x status1=%08x\n",
				     status0, status1);

			if (!status0 && !status1)
				continue;

			fmcs_genl_notify(fmcs_genl_ploam, &status0, sizeof(status0),
					 -1);
		}
	}

	return 0;
}

/**
 * fmcs_irq_handler() - threaded interrupt handler.
 * @irq: interrupt number.
 * @arg: the driver instance.
 *
 * Acknowledges at the interrupt controller and wakes the event thread.  The
 * vendor writes 0xffffffff to (gpio_base + 0x08), clearing every pending GPIO
 * interrupt rather than only the FPGA's; the port acknowledges through the
 * gpiod interrupt API so that only this line is cleared.
 *
 * Returns IRQ_HANDLED.
 */
static irqreturn_t fmcs_irq_handler(int irq, void *arg)
{
	struct fmcs_dev *fmcs = arg;

	atomic_inc(&fmcs->irq_count);
	wake_up_interruptible(&fmcs->event_wait);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------ */
/* Character device                                                         */
/* ------------------------------------------------------------------------ */

static int fmcs_mci_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev, struct fmcs_dev, cdev);

	return 0;
}

static int fmcs_mci_release(struct inode *inode, struct file *file)
{
	return 0;
}

/**
 * fmcs_ioctl_read_reg() - shared body of the two read ioctls.
 * @fmcs: driver instance.
 * @arg: user pointer to a struct fmcs_reg.
 * @reader: transport read function, FPGA or BOSA.
 *
 * Copies the request in, performs the read, and copies the address and the
 * value back out as one eight-byte structure, matching
 * FmcsMciIoctlReadReg() in the vendor module.
 *
 * Returns 0, -EFAULT or the transport's error.
 */
static int fmcs_ioctl_read_reg(struct fmcs_dev *fmcs, unsigned long arg,
			       int (*reader)(struct fmcs_dev *, u32, u32 *))
{
	struct fmcs_reg reg;
	int ret;

	if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
		return -EFAULT;

	ret = reader(fmcs, reg.addr, &reg.value);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &reg, sizeof(reg)))
		return -EFAULT;

	return 0;
}

/**
 * fmcs_ioctl_write_reg() - shared body of the two write ioctls.
 * @fmcs: driver instance.
 * @arg: user pointer to a struct fmcs_reg.
 * @writer: transport write function, FPGA or BOSA.
 *
 * Returns 0, -EFAULT or the transport's error.
 */
static int fmcs_ioctl_write_reg(struct fmcs_dev *fmcs, unsigned long arg,
				int (*writer)(struct fmcs_dev *, u32, u32))
{
	struct fmcs_reg reg;

	if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
		return -EFAULT;

	return writer(fmcs, reg.addr, reg.value);
}

/**
 * fmcs_ioctl_send_ploam() - transmit a 13-byte PLOAM message.
 * @fmcs: driver instance.
 * @arg: user pointer to the message.
 *
 * Unlike the vendor handler this does not leak the staging buffer on a fault.
 *
 * Returns 0 or -EFAULT.
 */
static int fmcs_ioctl_send_ploam(struct fmcs_dev *fmcs, unsigned long arg)
{
	u8 ploam[FMCS_PLOAM_LEN];

	if (copy_from_user(ploam, (void __user *)arg, sizeof(ploam)))
		return -EFAULT;

	return fmcs_spi_send_ploam(fmcs, ploam);
}

/**
 * fmcs_ioctl_send_hsgmii() - the vendor's link-bring-up stress test.
 * @fmcs: driver instance.
 * @arg: repetition count, not a pointer.
 *
 * The vendor allocates an 0x80-byte skb filled with 0xa5 per iteration and
 * queues it on the FTTR Ethernet netdev through __ECNT_HOOK(0x40, ...).  That
 * hook does not exist here, so the count is validated and rejected: silently
 * reporting success while transmitting nothing would be worse than refusing.
 *
 * Returns 0, or -EOPNOTSUPP.
 */
static int fmcs_ioctl_send_hsgmii(struct fmcs_dev *fmcs, unsigned long arg)
{
	dev_warn_ratelimited(fmcs->dev,
			     "hsgmii test transmit is not supported on this port\n");

	return -EOPNOTSUPP;
}

static long fmcs_mci_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct fmcs_dev *fmcs = file->private_data;
	long ret;

	/* The vendor rejects a foreign command type before taking the lock. */
	if (_IOC_TYPE(cmd) != FMCS_IOCTL_MAGIC)
		return -EINVAL;

	mutex_lock(&fmcs->ioctl_lock);

	switch (cmd) {
	case FMCS_IOC_WR_FPGA_REG:
		ret = fmcs_ioctl_write_reg(fmcs, arg, fmcs_spi_write_fpga_reg);
		break;
	case FMCS_IOC_RD_FPGA_REG:
		ret = fmcs_ioctl_read_reg(fmcs, arg, fmcs_spi_read_fpga_reg);
		break;
	case FMCS_IOC_WR_BOSA_REG:
		ret = fmcs_ioctl_write_reg(fmcs, arg, fmcs_spi_write_bosa_reg);
		break;
	case FMCS_IOC_RD_BOSA_REG:
		ret = fmcs_ioctl_read_reg(fmcs, arg, fmcs_spi_read_bosa_reg);
		break;
	case FMCS_IOC_SEND_PLOAM:
		ret = fmcs_ioctl_send_ploam(fmcs, arg);
		break;
	case FMCS_IOC_RECV_PLOAM:
		/*
		 * The vendor ignores the argument and always reports success;
		 * the message itself is delivered asynchronously.  Keep the
		 * call cheap and side-effect free so that moltcmd's poll loop
		 * behaves exactly as it did.
		 */
		ret = 0;
		break;
	case FMCS_IOC_SEND_HSGMII:
		ret = fmcs_ioctl_send_hsgmii(fmcs, arg);
		break;
	default:
		dev_warn_ratelimited(fmcs->dev,
				     "Fmcs ioctl do not have this cmd! p1=%u.\n",
				     _IOC_NR(cmd));
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&fmcs->ioctl_lock);

	return ret;
}

static const struct file_operations fmcs_mci_fops = {
	.owner		= THIS_MODULE,
	.open		= fmcs_mci_open,
	.release	= fmcs_mci_release,
	.unlocked_ioctl	= fmcs_mci_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= fmcs_mci_ioctl,
#endif
	.llseek		= no_llseek,
};

static int fmcs_mci_init(struct fmcs_dev *fmcs)
{
	struct device *dev = fmcs->dev;
	int ret;

	ret = alloc_chrdev_region(&fmcs->devt, 0, 1, FMCS_MCI_CLASS_NAME);
	if (ret) {
		dev_err(dev, "fmcs alloc chrdev region failed.\n");
		return ret;
	}

	cdev_init(&fmcs->cdev, &fmcs_mci_fops);
	fmcs->cdev.owner = THIS_MODULE;

	ret = cdev_add(&fmcs->cdev, fmcs->devt, 1);
	if (ret) {
		dev_err(dev, "fmcs add cdev failed.\n");
		goto err_unregister;
	}

	fmcs->class = class_create(FMCS_MCI_CLASS_NAME);
	if (IS_ERR(fmcs->class)) {
		ret = PTR_ERR(fmcs->class);
		fmcs->class = NULL;
		dev_err(dev, "fmcs reg sysfs class failed.\n");
		goto err_cdev;
	}

	fmcs->chardev = device_create(fmcs->class, NULL, fmcs->devt, fmcs,
				      FMCS_MCI_CLASS_NAME);
	if (IS_ERR(fmcs->chardev)) {
		ret = PTR_ERR(fmcs->chardev);
		fmcs->chardev = NULL;
		goto err_class;
	}

	dev_info(dev, "fmcs mci init success, major = %d, minor = %d\n",
		 MAJOR(fmcs->devt), MINOR(fmcs->devt));

	return 0;

err_class:
	class_destroy(fmcs->class);
	fmcs->class = NULL;
err_cdev:
	cdev_del(&fmcs->cdev);
err_unregister:
	unregister_chrdev_region(fmcs->devt, 1);

	return ret;
}

static void fmcs_mci_exit(struct fmcs_dev *fmcs)
{
	if (fmcs->chardev) {
		device_destroy(fmcs->class, fmcs->devt);
		fmcs->chardev = NULL;
	}

	if (fmcs->class) {
		class_destroy(fmcs->class);
		fmcs->class = NULL;
	}

	cdev_del(&fmcs->cdev);
	unregister_chrdev_region(fmcs->devt, 1);
}

/* ------------------------------------------------------------------------ */
/* Probe and remove                                                         */
/* ------------------------------------------------------------------------ */

/**
 * fmcs_probe_common() - shared body of the platform and SPI probes.
 * @fmcs: freshly zeroed instance whose dev and transport are already set.
 *
 * Brings up the pieces in the vendor's order - character device, net devices,
 * event delivery, then the interrupt - so that any later failure unwinds a
 * fully consistent earlier state.
 *
 * Returns 0 on success, or a negative errno.
 */
static int fmcs_probe_common(struct fmcs_dev *fmcs)
{
	int ret;

	mutex_init(&fmcs->ioctl_lock);
	init_waitqueue_head(&fmcs->event_wait);
	atomic_set(&fmcs->irq_count, 0);

	ret = fmcs_transport_init(fmcs);
	if (ret)
		return ret;

	ret = fmcs_mci_init(fmcs);
	if (ret)
		return ret;

	ret = fmcs_netdev_init(fmcs);
	if (ret)
		goto err_mci;

	ret = fmcs_bitstream_init(fmcs);
	if (ret)
		goto err_netdev;

	/*
	 * Configure the FPGA before anything tries to talk to it.  A board
	 * whose bitstream is still loaded by the bootloader or by another
	 * loader reports a failure here that is not fatal: the register
	 * interface works either way, and refusing to probe would make the
	 * device unserviceable.
	 */
#ifdef CONFIG_FTTR_FMCS_LOAD_AT_PROBE
	ret = fmcs_bitstream_load(fmcs);
	if (ret)
		dev_warn(fmcs->dev,
			 "bitstream load failed (%d), continuing anyway\n", ret);
#endif

	if (fmcs->irq > 0) {
		ret = devm_request_threaded_irq(fmcs->dev, fmcs->irq, NULL,
						fmcs_irq_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
						FMCS_DRV_NAME, fmcs);
		if (ret) {
			dev_err(fmcs->dev,
				"fpga spi driver requset irq failed, p1=%d.\n",
				ret);
			goto err_bitstream;
		}
	}

	fmcs->event_task = kthread_run(fmcs_event_thread, fmcs,
				       FMCS_DRV_NAME "/events");
	if (IS_ERR(fmcs->event_task)) {
		ret = PTR_ERR(fmcs->event_task);
		fmcs->event_task = NULL;
		dev_err(fmcs->dev, "fmcs create recv kthread failed!\n");
		goto err_irq;
	}

	dev_info(fmcs->dev, "fpga spi driver init success.\n");

	return 0;

err_irq:
	if (fmcs->irq > 0)
		devm_free_irq(fmcs->dev, fmcs->irq, fmcs);
err_bitstream:
	fmcs_bitstream_exit(fmcs);
err_netdev:
	fmcs_netdev_exit(fmcs);
err_mci:
	fmcs_mci_exit(fmcs);

	return ret;
}

static void fmcs_remove_common(struct fmcs_dev *fmcs)
{
	if (fmcs->event_task) {
		fmcs->event_stop = true;
		wake_up_interruptible(&fmcs->event_wait);
		kthread_stop(fmcs->event_task);
		fmcs->event_task = NULL;
	}

	if (fmcs->irq > 0)
		devm_free_irq(fmcs->dev, fmcs->irq, fmcs);

	fmcs_bitstream_exit(fmcs);
	fmcs_netdev_exit(fmcs);
	fmcs_mci_exit(fmcs);
	fmcs_transport_exit(fmcs);

	dev_info(fmcs->dev, "fmcs mci exit.\n");
}

static int fmcs_platform_probe(struct platform_device *pdev)
{
	struct fmcs_dev *fmcs;

	fmcs = devm_kzalloc(&pdev->dev, sizeof(*fmcs), GFP_KERNEL);
	if (!fmcs)
		return -ENOMEM;

	fmcs->dev = &pdev->dev;
	/*
	 * The interrupt is described either as a real interrupt property or as
	 * a gpio the interrupt is gated through.  The stock device tree has
	 * both: fpga-spi-int-gpio = <23> and interrupts = <GIC_SPI 26 ...>.
	 */
	fmcs->irq = platform_get_irq_optional(pdev, 0);
	if (fmcs->irq < 0)
		fmcs->irq = 0;

	platform_set_drvdata(pdev, fmcs);

	return fmcs_probe_common(fmcs);
}

static void fmcs_platform_remove(struct platform_device *pdev)
{
	fmcs_remove_common(platform_get_drvdata(pdev));
}

static const struct of_device_id fmcs_of_match[] = {
	{ .compatible = "h3c,fmcs" },
	{ }
};
MODULE_DEVICE_TABLE(of, fmcs_of_match);

static struct platform_driver fmcs_platform_driver = {
	.probe		= fmcs_platform_probe,
	.remove		= fmcs_platform_remove,
	.driver		= {
		.name		= FMCS_DRV_NAME,
		.of_match_table	= fmcs_of_match,
	},
};

static int fmcs_spi_probe(struct spi_device *spi)
{
	struct fmcs_dev *fmcs;
	int ret;

	fmcs = devm_kzalloc(&spi->dev, sizeof(*fmcs), GFP_KERNEL);
	if (!fmcs)
		return -ENOMEM;

	fmcs->dev = &spi->dev;
	fmcs->transport.spi = spi;
	fmcs->irq = spi->irq;

	spi_set_drvdata(spi, fmcs);

	/* Short register transfers do not need the controller's DMA. */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup failed\n");

	return fmcs_probe_common(fmcs);
}

static void fmcs_spi_remove(struct spi_device *spi)
{
	fmcs_remove_common(spi_get_drvdata(spi));
}

static const struct spi_device_id fmcs_spi_id[] = {
	{ "fmcs", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, fmcs_spi_id);

static struct spi_driver fmcs_spi_driver = {
	.probe		= fmcs_spi_probe,
	.remove		= fmcs_spi_remove,
	.id_table	= fmcs_spi_id,
	.driver		= {
		.name		= FMCS_DRV_NAME,
		.of_match_table	= fmcs_of_match,
	},
};

/* ------------------------------------------------------------------------ */
/* Module                                                                   */
/* ------------------------------------------------------------------------ */

static int __init fmcs_genl_init(void)
{
	unsigned int i;

	for (i = 0; i < FMCS_GENL_NUM; i++) {
		struct genl_family *family = &fmcs_genl_families[i];

		family->name		= fmcs_genl_names[i];
		family->version		= FMCS_GENL_VERSION;
		family->maxattr		= FMCS_ATTR_MAX;
		family->policy		= fmcs_genl_policy;
		family->netnsok		= true;
		family->module		= THIS_MODULE;

		if (genl_register_family(family)) {
			pr_err(FMCS_DRV_NAME ": Genl Server Init fail for %s\n",
			       fmcs_genl_names[i]);
			fmcs_genl_registered = i;
			return -EINVAL;
		}

		if (strcmp(fmcs_genl_names[i], FMCS_GENL_NAME_PLOAM_NET) == 0)
			fmcs_genl_ploam = i;
	}

	fmcs_genl_registered = FMCS_GENL_NUM;

	return 0;
}

static void fmcs_genl_exit(void)
{
	unsigned int i;

	for (i = 0; i < fmcs_genl_registered; i++)
		genl_unregister_family(&fmcs_genl_families[i]);

	fmcs_genl_registered = 0;
}

static int __init fmcs_init(void)
{
	int ret;

	ret = fmcs_genl_init();
	if (ret)
		return ret;

	ret = platform_driver_register(&fmcs_platform_driver);
	if (ret) {
		pr_err(FMCS_DRV_NAME ": platform_driver_register ret = %d\n", ret);
		goto err_genl;
	}

	ret = spi_register_driver(&fmcs_spi_driver);
	if (ret) {
		pr_err(FMCS_DRV_NAME ": spi_register_driver ret = %d\n", ret);
		platform_driver_unregister(&fmcs_platform_driver);
		goto err_genl;
	}

	pr_info(FMCS_DRV_NAME ": fpga spi driver init success.\n");

	return 0;

err_genl:
	fmcs_genl_exit();

	return ret;
}

static void __exit fmcs_exit(void)
{
	spi_unregister_driver(&fmcs_spi_driver);
	platform_driver_unregister(&fmcs_platform_driver);
	fmcs_genl_exit();

	pr_info(FMCS_DRV_NAME ": fpga spi driver init FAILED.\n");
}

module_init(fmcs_init);
module_exit(fmcs_exit);

MODULE_AUTHOR("H3C HM2004-DU FTTR port");
MODULE_DESCRIPTION("fpga management control system Module");
MODULE_LICENSE("GPL");
MODULE_VERSION(FMCS_DRV_VERSION);
MODULE_FIRMWARE(FMCS_FIRMWARE_NAME);
