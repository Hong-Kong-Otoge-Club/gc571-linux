// SPDX-License-Identifier: GPL-2.0
/*
 * gc571sig - query HDMI input signal info from AVerMedia GC571 capture cards.
 *
 * The GC571 exposes a vendor command mailbox via UVC Extension Unit 4 on the
 * VideoControl interface. uvcvideo does not register that XU (it is sourced
 * from the Output Terminal, which the chain scanner drops), so this companion
 * module talks to endpoint 0 directly - no interface claiming, no uvcvideo
 * changes, safe to run while the card is streaming.
 *
 * Mailbox protocol (reverse engineered from Assist Central Pro):
 *   selector 1 = command / response buffer (variable length)
 *   selector 2 = 16-bit register: SET resizes selector 1, GET reports the
 *                pending response length
 *   To read register R:
 *     SET_CUR(sel2) <- 9      resize selector 1 to fit the command
 *     SET_CUR(sel1) <- 9-byte command frame
 *     GET_CUR(sel2) -> L      response length (poll until ready)
 *     GET_CUR(sel1) -> L bytes
 *   9-byte command frame: [toggle][0x06][00][00][reg][00][00][00][checksum]
 *   The toggle byte (0xA0/0xA1) must differ from the previous command or the
 *   device ignores it; the response echoes the toggle.
 *   checksum = -sum(bytes[0..n-2]) & 0xFF  (whole frame sums to 0 mod 256)
 *
 * Read the decoded signal info per card at /sys/kernel/debug/gc571sig/<serial>.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define GC571_VID	0x07ca
#define GC571_PID	0x0571
#define XU_UNIT		4
#define XU_INTF		0

#define UVC_SET_CUR	0x01
#define UVC_GET_CUR	0x81

#define RT_GET		0xA1	/* class | interface | dev-to-host */
#define RT_SET		0x21	/* class | interface | host-to-dev */

#define SEL_CMD		1	/* command / response buffer */
#define SEL_STATUS	2	/* resize / response-length register */

#define REG_TIMING	0x37	/* HDMI raw timing block */
#define CMD_LEN		9	/* command frame size */

#define MAX_DEV		8

struct gc571_card {
	char serial[40];
	u8 next_toggle;
	struct dentry *file;
};

static struct gc571_card cards[MAX_DEV];
static int ncards;
static DEFINE_MUTEX(query_lock);
static struct dentry *dbg_dir;

struct gc571_timing {
	u16 width, height, htotal, vtotal, refresh;	/* refresh in 0.01 Hz */
	bool cksum_ok;
};

static u8 mb_cksum(const u8 *b, int n)
{
	int i, s = 0;

	for (i = 0; i < n; i++)
		s += b[i];
	return (u8)(-s);
}

static void decode_timing(const u8 *b, int n, struct gc571_timing *t)
{
	memset(t, 0, sizeof(*t));
	if (n < 0x14)
		return;
	t->vtotal   = b[0x06] | (b[0x07] << 8);
	t->htotal   = b[0x08] | (b[0x09] << 8);
	t->height   = b[0x0a] | (b[0x0b] << 8);
	t->width    = b[0x0c] | (b[0x0d] << 8);
	t->refresh  = b[0x12] | (b[0x13] << 8);
	t->cksum_ok = mb_cksum(b, n - 1) == b[n - 1];
}

/* GET a 2-byte selector value (GET_CUR). */
static int xu_get_word(struct usb_device *udev, u8 sel, u16 *val)
{
	u8 b[2];
	int ret = usb_control_msg_recv(udev, 0, UVC_GET_CUR, RT_GET, sel << 8,
				       (XU_UNIT << 8) | XU_INTF, b, 2, 1000,
				       GFP_KERNEL);
	if (ret)
		return ret;
	*val = b[0] | (b[1] << 8);
	return 0;
}

/* Writing a byte count to selector 2 resizes selector 1's length. */
static int xu_set_sel1_len(struct usb_device *udev, u16 len)
{
	u8 b[2] = { len & 0xff, (len >> 8) & 0xff };

	return usb_control_msg_send(udev, 0, UVC_SET_CUR, RT_SET,
				    SEL_STATUS << 8, (XU_UNIT << 8) | XU_INTF,
				    b, sizeof(b), 1000, GFP_KERNEL);
}

static int xu_set_cmd(struct usb_device *udev, u8 reg, u8 tog)
{
	u8 cmd[CMD_LEN] = { tog, 0x06, 0x00, 0x00, reg, 0x00, 0x00, 0x00, 0 };

	cmd[8] = mb_cksum(cmd, 8);
	return usb_control_msg_send(udev, 0, UVC_SET_CUR, RT_SET, SEL_CMD << 8,
				    (XU_UNIT << 8) | XU_INTF, cmd, sizeof(cmd),
				    1000, GFP_KERNEL);
}

/* A fresh response echoes our toggle byte and carries a valid frame checksum. */
static bool resp_is_fresh(const u8 *b, int n, u8 tog)
{
	return n >= 8 && b[0] == tog && mb_cksum(b, n - 1) == b[n - 1];
}

static int xu_read_reg(struct usb_device *udev, u8 reg, u8 *togp, u8 *resp,
		       int respcap, int *rlen_out)
{
	int ret, pass, i;
	u16 rlen;
	u8 tog;

	/*
	 * The device only processes a command whose toggle bit differs from
	 * the previous one it saw. *togp tracks the next value to use per
	 * card, so the first pass usually lands; flipping across passes
	 * still guarantees a fresh one within two. Freshness is confirmed
	 * via the echoed toggle + checksum.
	 */
	for (pass = 0; pass < 4; pass++) {
		tog = *togp;
		*togp ^= 0x01;

		ret = xu_set_sel1_len(udev, CMD_LEN);
		if (ret)
			return ret;
		ret = xu_set_cmd(udev, reg, tog);
		if (ret)
			return ret;

		for (i = 0; i < 30; i++) {
			usleep_range(7000, 9000);
			ret = xu_get_word(udev, SEL_STATUS, &rlen);
			if (ret)
				continue;
			if (rlen < 8 || rlen > respcap)
				continue;
			ret = usb_control_msg_recv(udev, 0, UVC_GET_CUR, RT_GET,
						   SEL_CMD << 8,
						   (XU_UNIT << 8) | XU_INTF,
						   resp, rlen, 1000,
						   GFP_KERNEL);
			if (ret)
				continue;	/* length raced; re-poll */
			if (resp_is_fresh(resp, rlen, tog)) {
				*rlen_out = rlen;
				return 0;
			}
		}
	}
	return -EIO;
}

struct find_ctx {
	const char *serial;
	struct usb_device *udev;
};

static int match_serial(struct usb_device *udev, void *data)
{
	struct find_ctx *c = data;

	if (le16_to_cpu(udev->descriptor.idVendor) == GC571_VID &&
	    le16_to_cpu(udev->descriptor.idProduct) == GC571_PID &&
	    udev->serial && !strcmp(udev->serial, c->serial)) {
		c->udev = usb_get_dev(udev);
		return 1;
	}
	return 0;
}

/* Locate the still-present card by serial and read its timing block. */
static int gc571_read_timing(struct gc571_card *card, u8 *resp, int cap,
			     int *rlen)
{
	struct find_ctx c = { .serial = card->serial, .udev = NULL };
	int ret;

	usb_for_each_dev(&c, match_serial);
	if (!c.udev)
		return -ENODEV;
	ret = xu_read_reg(c.udev, REG_TIMING, &card->next_toggle, resp, cap,
			  rlen);
	usb_put_dev(c.udev);
	return ret;
}

static int sig_show(struct seq_file *s, void *v)
{
	struct gc571_card *card = s->private;
	struct gc571_timing t;
	u8 resp[64];
	int ret, rlen;

	mutex_lock(&query_lock);
	ret = gc571_read_timing(card, resp, sizeof(resp), &rlen);
	mutex_unlock(&query_lock);

	if (ret) {
		seq_printf(s, "error: query failed (%d)\n", ret);
		return 0;
	}
	decode_timing(resp, rlen, &t);
	if (!t.width || !t.height) {
		seq_puts(s, "no signal\n");
		return 0;
	}
	seq_printf(s, "%ux%u @ %u.%02u Hz  Htotal=%u Vtotal=%u  checksum=%s\n",
		   t.width, t.height, t.refresh / 100, t.refresh % 100,
		   t.htotal, t.vtotal, t.cksum_ok ? "ok" : "bad");
	return 0;
}

static int sig_open(struct inode *inode, struct file *file)
{
	return single_open(file, sig_show, inode->i_private);
}

static const struct file_operations sig_fops = {
	.owner	 = THIS_MODULE,
	.open	 = sig_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static int collect(struct usb_device *udev, void *data)
{
	if (le16_to_cpu(udev->descriptor.idVendor) == GC571_VID &&
	    le16_to_cpu(udev->descriptor.idProduct) == GC571_PID &&
	    ncards < MAX_DEV) {
		strscpy(cards[ncards].serial,
			udev->serial ? udev->serial : "unknown",
			sizeof(cards[ncards].serial));
		cards[ncards].next_toggle = 0xA1;
		ncards++;
	}
	return 0;
}

static int __init gc571sig_init(void)
{
	struct gc571_timing t;
	u8 resp[64];
	int i, rlen;

	usb_for_each_dev(NULL, collect);
	if (!ncards) {
		pr_warn("gc571sig: no GC571 (07ca:0571) found\n");
		return 0;
	}

	dbg_dir = debugfs_create_dir("gc571sig", NULL);

	for (i = 0; i < ncards; i++) {
		cards[i].file = debugfs_create_file(cards[i].serial, 0444,
						    dbg_dir, &cards[i],
						    &sig_fops);

		mutex_lock(&query_lock);
		if (gc571_read_timing(&cards[i], resp, sizeof(resp),
				      &rlen) == 0) {
			decode_timing(resp, rlen, &t);
			pr_info("gc571sig: %s  %ux%u @ %u.%02u Hz\n",
				cards[i].serial, t.width, t.height,
				t.refresh / 100, t.refresh % 100);
		} else {
			pr_info("gc571sig: %s  initial query failed\n",
				cards[i].serial);
		}
		mutex_unlock(&query_lock);
	}
	pr_info("gc571sig: ready, %d card(s) at /sys/kernel/debug/gc571sig/\n",
		ncards);
	return 0;
}

static void __exit gc571sig_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	pr_info("gc571sig: unloaded\n");
}

module_init(gc571sig_init);
module_exit(gc571sig_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("streampc");
MODULE_DESCRIPTION("AVerMedia GC571 HDMI input signal query via UVC XU mailbox");
