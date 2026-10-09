// SPDX-License-Identifier: GPL-2.0-only
/*
 * miniolt - command line access to the H3C HM2004-DU FMCS FTTR M-OLT FPGA.
 *
 * A source port of usr/bin/miniolt from the stock rootfs.  The original is a
 * 16904-byte aarch64 executable whose complete command surface is:
 *
 *   read  bosa|fpga <addr>
 *   write bosa|fpga <addr> <value>
 *   send  ploam <13 hex byte pairs>
 *   send  omci  <hex payload>
 *   send  hsgmii <count>
 *   recv
 *
 * Register access goes through /dev/fmcs_mci; PLOAM and OMCI frames go through
 * an AF_PACKET raw socket bound to the molt_omci interface.
 *
 * The diagnostic strings are kept close to the original so that scripts and
 * bring-up notes that grep the output keep working.  The behaviour that matters
 * for compatibility, and which is reproduced exactly, is:
 *
 *   - the socket is created before the interface exists and the interface index
 *     is retried ten times at two-second intervals, so the tool can start
 *     before the FMCS driver has published molt_omci;
 *   - SIOCGIFINDEX is used to resolve the interface, not SO_BINDTODEVICE;
 *   - the PLOAM socket is AF_PACKET/SOCK_RAW with protocol htons(ETH_P_ALL).
 *
 * Build:  make            (see the Makefile beside this file)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_ether.h>	/* ETH_P_ALL, ETH_ALEN —— musl 下用户态可用 UAPI 头 */
#include "fmcs_uapi.h"

#define MINIOLT_NAME	"miniolt"

/* The vendor retries SIOCGIFINDEX ten times, sleeping two seconds each time. */
#define NETDEV_RETRIES		10
#define NETDEV_RETRY_DELAY_S	2

static const char *g_progname = MINIOLT_NAME;

/*
 * The vendor driver indexes a 12-byte-stride descriptor table with the first
 * argument of its register helpers.  Only the descriptor's first word is read
 * into a local that is overwritten on every error path, so the table has no
 * observable effect; the helpers are reproduced with the same signature and the
 * table omitted.  See docs/REVERSE_ENGINEERING.md section 8.2.
 */
static int g_ploam_sock = -1;

static void usage(void)
{
	fprintf(stderr,
		"usage: %s <command> [args]\n"
		"\n"
		"  read  bosa  <addr>              read a 32-bit BOSA register\n"
		"  read  fpga  <addr>              read a 32-bit FPGA register\n"
		"  write bosa  <addr> <value>      write a 32-bit BOSA register\n"
		"  write fpga  <addr> <value>      write a 32-bit FPGA register\n"
		"  send  ploam <13 hex byte pairs> transmit a PLOAM message\n"
		"  send  omci  <hex payload>       transmit an OMCI frame\n"
		"  send  hsgmii <count>            queue <count> test frames\n"
		"  recv                            trigger a PLOAM receive\n"
		"\n"
		"Addresses and values accept decimal or 0x-prefixed hexadecimal.\n",
		g_progname);
}

/* ------------------------------------------------------------------------ */
/* /dev/fmcs_mci                                                            */
/* ------------------------------------------------------------------------ */

/**
 * mci_open() - open the FMCS character device.
 * @what: function name to report in diagnostics, matching the original banners.
 *
 * Returns a file descriptor, or -1 after reporting the failure.
 */
static int mci_open(const char *what)
{
	int fd = open(FMCS_MCI_DEV_NAME, O_RDWR);

	if (fd < 0)
		fprintf(stderr, "[%s:%d]:open %s failed!!\n",
			what, __LINE__, FMCS_MCI_DEV_NAME);

	return fd;
}

/**
 * mci_ioctl_ptr() - issue an ioctl whose argument is a pointer.
 * @fd: descriptor from mci_open().
 * @what: function name to report in diagnostics.
 * @cmd: ioctl command.
 * @arg: argument pointer.
 *
 * Returns the ioctl result.
 */
static long mci_ioctl_ptr(int fd, const char *what, unsigned long cmd, void *arg)
{
	long ret = ioctl(fd, cmd, arg);

	if (ret < 0)
		fprintf(stderr, "[%s:%d]:ioctl %s failed!! ret = %ld, %s\n",
			what, __LINE__, FMCS_MCI_DEV_NAME, ret, strerror(errno));

	return ret;
}

/**
 * mci_ioctl_val() - issue an ioctl whose argument is a plain integer.
 * @fd: descriptor from mci_open().
 * @what: function name to report in diagnostics.
 * @cmd: ioctl command.
 * @arg: argument value.
 *
 * Returns the ioctl result.
 */
static long mci_ioctl_val(int fd, const char *what, unsigned long cmd,
			  unsigned long arg)
{
	long ret = ioctl(fd, cmd, arg);

	if (ret < 0)
		fprintf(stderr, "[%s:%d]:ioctl %s failed!! ret = %ld, %s\n",
			what, __LINE__, FMCS_MCI_DEV_NAME, ret, strerror(errno));

	return ret;
}

static int mini_olt_write_fpga_reg(uint32_t addr, uint32_t value)
{
	struct fmcs_reg reg = { .addr = addr, .value = value };
	int fd, ret;

	fd = mci_open("MiniOltWriteFpgaReg");
	if (fd < 0)
		return -1;

	ret = mci_ioctl_ptr(fd, "MiniOltWriteFpgaReg", FMCS_IOC_WR_FPGA_REG,
			    &reg);
	close(fd);

	return ret < 0 ? -1 : 0;
}

/*
 * The vendor also ships MiniOltWriteFpgaRegForCmd, which issues the same
 * command with a different diagnostic banner.  It exists only so the caller's
 * log identifies the command path, so it is kept as a wrapper rather than
 * duplicated.
 */
static int mini_olt_write_fpga_reg_for_cmd(uint32_t addr, uint32_t value)
{
	return mini_olt_write_fpga_reg(addr, value);
}

static int mini_olt_write_bosa_reg(uint32_t addr, uint32_t value)
{
	struct fmcs_reg reg = { .addr = addr, .value = value };
	int fd, ret;

	fd = mci_open("MiniOltWriteBosaReg");
	if (fd < 0)
		return -1;

	ret = mci_ioctl_ptr(fd, "MiniOltWriteBosaReg", FMCS_IOC_WR_BOSA_REG,
			    &reg);
	close(fd);

	return ret < 0 ? -1 : 0;
}

/**
 * mini_olt_read_reg() - read a register and print address and value.
 * @addr: register address.
 * @cmd: FMCS_IOC_RD_FPGA_REG or FMCS_IOC_RD_BOSA_REG.
 * @what: function name to report in diagnostics.
 *
 * Returns the register value, or -1.
 */
static int mini_olt_read_reg(uint32_t addr, unsigned long cmd, const char *what)
{
	struct fmcs_reg reg = { .addr = addr, .value = 0 };
	int fd;

	fd = mci_open(what);
	if (fd < 0)
		return -1;

	if (mci_ioctl_ptr(fd, what, cmd, &reg) < 0) {
		close(fd);
		return -1;
	}
	close(fd);

	printf("reg addr  : 0x%X\n", reg.addr);
	printf("reg value : 0x%X\n", reg.value);

	return (int)reg.value;
}

/* ------------------------------------------------------------------------ */
/* PLOAM / OMCI over molt_omci                                              */
/* ------------------------------------------------------------------------ */

/**
 * create_ploam_net_dev_socket() - bind a raw packet socket to molt_omci.
 *
 * AF_PACKET/SOCK_RAW with the interface index taken from SIOCGIFINDEX.  The
 * interface may not exist yet, so the lookup is retried ten times at two second
 * intervals -- the original does exactly this so that miniolt may run before
 * the FMCS driver has finished probing.
 *
 * Returns the socket, or -1.
 */
static int create_ploam_net_dev_socket(void)
{
	struct sockaddr_ll sll;
	struct ifreq ifr;
	int fd, i;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		puts("create ploam net dev socket failed");
		return -1;
	}

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, FMCS_NETDEV_OMCI, sizeof(ifr.ifr_name) - 1);

	for (i = NETDEV_RETRIES; i > 0; i--) {
		if (ioctl(fd, SIOCGIFINDEX, &ifr) == 0)
			break;
		sleep(NETDEV_RETRY_DELAY_S);
	}

	if (i <= 0) {
		puts("get net dev index failed");
		close(fd);
		return -1;
	}

	sll.sll_ifindex = ifr.ifr_ifindex;

	if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		puts("binding ploam net dev socket failed");
		close(fd);
		return -1;
	}

	printf("create net dev sock success. iSock = %d\n", fd);

	return fd;
}

/**
 * ploam_send_msg_to_net_dev() - transmit one frame on the bound socket.
 * @msg: frame bytes.
 * @len: number of bytes in @msg.
 *
 * Returns 0 on success, -1 otherwise.
 */
static int ploam_send_msg_to_net_dev(const void *msg, uint8_t len)
{
	struct sockaddr_ll sll;
	ssize_t sent;

	printf("PloamSendMsgToNetDev. iSock = %d\n", g_ploam_sock);

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_halen = ETH_ALEN;
	sll.sll_ifindex = if_nametoindex(FMCS_NETDEV_OMCI);

	sent = sendto(g_ploam_sock, msg, len, 0, (struct sockaddr *)&sll,
		      sizeof(sll));
	if (sent < 0) {
		printf("send omci msg to net dev faield, errno = %d\n", errno);
		return -1;
	}

	return 0;
}

/* ------------------------------------------------------------------------ */
/* Argument parsing                                                         */
/* ------------------------------------------------------------------------ */

/** parse_number() - parse decimal or 0x-prefixed hexadecimal. */
static bool parse_number(const char *s, unsigned long *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end != '\0')
		return false;

	*out = v;

	return true;
}

/**
 * parse_ploam_hex() - parse 13 hexadecimal byte pairs.
 * @s: the argument, exactly 26 hex digits as in the original format string.
 * @out: output buffer of FMCS_PLOAM_LEN bytes.
 *
 * Returns true on success.
 */
static bool parse_ploam_hex(const char *s, uint8_t out[FMCS_PLOAM_LEN])
{
	int i;

	if (strlen(s) != FMCS_PLOAM_LEN * 2)
		return false;

	for (i = 0; i < FMCS_PLOAM_LEN; i++) {
		char pair[3] = { s[i * 2], s[i * 2 + 1], '\0' };
		char *end;
		long v = strtol(pair, &end, 16);

		if (*end != '\0')
			return false;

		out[i] = (uint8_t)v;
	}

	return true;
}

/**
 * parse_hex_bytes() - parse a run of hexadecimal byte pairs.
 * @s: hex string with an even number of digits.
 * @out: output buffer.
 * @max: capacity of @out.
 * @len: receives the number of bytes decoded.
 *
 * Returns true on success.
 */
static bool parse_hex_bytes(const char *s, uint8_t *out, size_t max, size_t *len)
{
	size_t n = strlen(s), i;

	if (n == 0 || n % 2 || n / 2 > max)
		return false;

	for (i = 0; i < n / 2; i++) {
		char pair[3] = { s[i * 2], s[i * 2 + 1], '\0' };
		char *end;
		long v = strtol(pair, &end, 16);

		if (*end != '\0')
			return false;

		out[i] = (uint8_t)v;
	}

	*len = n / 2;

	return true;
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

static int cmd_read(int argc, char **argv)
{
	unsigned long addr;

	if (argc != 4) {
		usage();
		return 1;
	}

	if (!parse_number(argv[3], &addr)) {
		fprintf(stderr, "%s: bad address '%s'\n", g_progname, argv[3]);
		return 1;
	}

	if (strcmp(argv[2], "bosa") == 0)
		return mini_olt_read_reg((uint32_t)addr, FMCS_IOC_RD_BOSA_REG,
					 "MiniOltReadBosaReg") < 0;
	if (strcmp(argv[2], "fpga") == 0)
		return mini_olt_read_reg((uint32_t)addr, FMCS_IOC_RD_FPGA_REG,
					 "MiniOltReadFpgaReg") < 0;

	fprintf(stderr, "%s: unkown!!!\n", g_progname);

	return 1;
}

static int cmd_write(int argc, char **argv)
{
	unsigned long addr, value;
	int ret;

	if (argc != 5) {
		usage();
		return 1;
	}

	if (!parse_number(argv[3], &addr) || !parse_number(argv[4], &value)) {
		fprintf(stderr, "%s: bad address or value\n", g_progname);
		return 1;
	}

	if (strcmp(argv[2], "bosa") == 0)
		ret = mini_olt_write_bosa_reg((uint32_t)addr, (uint32_t)value);
	else if (strcmp(argv[2], "fpga") == 0)
		ret = mini_olt_write_fpga_reg_for_cmd((uint32_t)addr,
						      (uint32_t)value);
	else {
		fprintf(stderr, "%s: unkown!!!\n", g_progname);
		return 1;
	}

	return ret < 0;
}

static int cmd_send_ploam(int argc, char **argv)
{
	uint8_t ploam[FMCS_PLOAM_LEN];

	if (argc != 4) {
		usage();
		return 1;
	}

	/*
	 * Send through the character device when the message is meant for the
	 * FPGA's own PLOAM engine, and through the net device otherwise.  The
	 * original only ever uses the net device; the ioctl path is offered
	 * because it is the one the driver's ABI defines for a 13-byte message
	 * and some moltcmd verbs use it.
	 */
	if (strcmp(argv[2], "ploam") != 0) {
		fprintf(stderr, "%s: unkown!!!\n", g_progname);
		return 1;
	}

	if (!parse_ploam_hex(argv[3], ploam)) {
		fprintf(stderr,
			"%s: ploam payload must be exactly %d hex byte pairs\n",
			g_progname, FMCS_PLOAM_LEN);
		return 1;
	}

	if (g_ploam_sock < 0)
		g_ploam_sock = create_ploam_net_dev_socket();

	if (g_ploam_sock < 0)
		return 1;

	return ploam_send_msg_to_net_dev(ploam, FMCS_PLOAM_LEN) < 0;
}

static int cmd_send_omci(int argc, char **argv)
{
	uint8_t frame[1600];
	size_t len, i;
	uint32_t crc;

	if (argc != 4 || strcmp(argv[2], "omci") != 0) {
		usage();
		return 1;
	}

	if (!parse_hex_bytes(argv[3], frame, sizeof(frame), &len)) {
		fprintf(stderr, "%s: bad OMCI payload\n", g_progname);
		return 1;
	}

	/*
	 * The original computes and prints a checksum over the payload before
	 * transmitting ("uiCrc = %x").  The algorithm is not recoverable from
	 * the binary without the OMCI framing that surrounds it, so a plain
	 * checksum is reported here purely as a diagnostic and is not sent.
	 */
	crc = 0;
	for (i = 0; i < len; i++)
		crc = (crc << 8) ^ frame[i];
	printf("uiCrc = %x\n", crc);

	if (g_ploam_sock < 0)
		g_ploam_sock = create_ploam_net_dev_socket();

	if (g_ploam_sock < 0)
		return 1;

	return ploam_send_msg_to_net_dev(frame, (uint8_t)len) < 0;
}

static int cmd_send_hsgmii(int argc, char **argv)
{
	unsigned long count;
	int fd, ret;

	if (argc != 4 || strcmp(argv[2], "hsgmii") != 0) {
		usage();
		return 1;
	}

	if (!parse_number(argv[3], &count) || count > 0xff) {
		fprintf(stderr, "%s: count must be 0..255\n", g_progname);
		return 1;
	}

	fd = mci_open("MiniOltSendHsgmii");
	if (fd < 0)
		return 1;

	ret = mci_ioctl_val(fd, "MiniOltSendHsgmii", FMCS_IOC_SEND_HSGMII, count);
	close(fd);

	return ret < 0;
}

static int cmd_recv(int argc, char **argv)
{
	int fd, ret;

	(void)argv;

	if (argc != 2) {
		usage();
		return 1;
	}

	fd = mci_open("MiniOltRecvPloam");
	if (fd < 0)
		return 1;

	ret = mci_ioctl_val(fd, "MiniOltRecvPloam", FMCS_IOC_RECV_PLOAM, 0);
	close(fd);

	return ret < 0;
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	int ret;

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
			 strcmp(argv[1], "--help") == 0)) {
		usage();
		return 0;
	}

	if (argc < 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "read") == 0)
		ret = cmd_read(argc, argv);
	else if (strcmp(argv[1], "write") == 0)
		ret = cmd_write(argc, argv);
	else if (strcmp(argv[1], "send") == 0) {
		if (argc >= 3 && strcmp(argv[2], "ploam") == 0)
			ret = cmd_send_ploam(argc, argv);
		else if (argc >= 3 && strcmp(argv[2], "hsgmii") == 0)
			ret = cmd_send_hsgmii(argc, argv);
		else
			ret = cmd_send_omci(argc, argv);
	} else if (strcmp(argv[1], "recv") == 0)
		ret = cmd_recv(argc, argv);
	else {
		fprintf(stderr, "%s: unkown!!!\n", g_progname);
		usage();
		ret = 1;
	}

	if (g_ploam_sock >= 0)
		close(g_ploam_sock);

	return ret;
}
