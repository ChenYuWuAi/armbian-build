/* ampreg - dump CS35L41 registers over I2C.
 *
 * The CS35L41 protocol is: 32-bit big-endian register address, then a
 * 32-bit big-endian value; reads are a write of the address followed by a
 * repeated-start read of 4 bytes.
 *
 * Static aarch64 build so the same binary runs under Armbian (glibc) and
 * Android (bionic):
 *   aarch64-linux-gnu-gcc-12 -static -O2 -o ampreg ampreg.c
 *
 * Usage:
 *   ampreg [-b bus[,bus...]] [-a addr[,addr]] [-r reg[-reg]] [-n] [--raw]
 *          [--set reg=val]
 * Examples:
 *   ampreg -b 1,3 -a 40,41,42,43 -r 0x4808,0x4840,0x6c04,0x6808
 *   ampreg -b 3 -a 40 --set 0x4840=0x10
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MAX_BUS 8
#define MAX_ADDR 16
#define MAX_REG 64

static int buses[MAX_BUS], nbuses;
static int addrs[MAX_ADDR], naddrs;
static unsigned int regs[MAX_REG];
static int nregs;
static unsigned int set_reg = 0xffffffff, set_val;

static int parse_list(const char *s, int *out, int max)
{
	char *dup = strdup(s), *p = dup, *tok;
	int n = 0;

	while ((tok = strsep(&p, ",")) != NULL && n < max) {
		if (*tok)
			out[n++] = (int)strtol(tok, NULL, 0);
	}
	free(dup);
	return n;
}

static int read_reg(int fd, int addr, unsigned int reg, unsigned int *val)
{
	unsigned char w[4], r[4];
	struct i2c_msg msgs[2];
	struct i2c_rdwr_ioctl_data xfer;

	w[0] = (reg >> 24) & 0xff;
	w[1] = (reg >> 16) & 0xff;
	w[2] = (reg >> 8) & 0xff;
	w[3] = reg & 0xff;

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = 4;
	msgs[0].buf = w;

	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 4;
	msgs[1].buf = r;

	xfer.msgs = msgs;
	xfer.nmsgs = 2;

	if (ioctl(fd, I2C_RDWR, &xfer) < 0)
		return -1;

	*val = ((unsigned)r[0] << 24) | ((unsigned)r[1] << 16) |
	       ((unsigned)r[2] << 8) | (unsigned)r[3];
	return 0;
}

static int write_reg(int fd, int addr, unsigned int reg, unsigned int val)
{
	unsigned char b[8];
	struct i2c_msg msg;
	struct i2c_rdwr_ioctl_data xfer;

	b[0] = (reg >> 24) & 0xff;
	b[1] = (reg >> 16) & 0xff;
	b[2] = (reg >> 8) & 0xff;
	b[3] = reg & 0xff;
	b[4] = (val >> 24) & 0xff;
	b[5] = (val >> 16) & 0xff;
	b[6] = (val >> 8) & 0xff;
	b[7] = val & 0xff;

	msg.addr = addr;
	msg.flags = 0;
	msg.len = 8;
	msg.buf = b;

	xfer.msgs = &msg;
	xfer.nmsgs = 1;

	return ioctl(fd, I2C_RDWR, &xfer) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
	int i, b, a, r;
	char path[64];

	/* defaults: all eight elish amps */
	buses[nbuses++] = 1;
	buses[nbuses++] = 3;
	for (a = 0x40; a <= 0x43; a++)
		addrs[naddrs++] = a;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc)
			nbuses = parse_list(argv[++i], buses, MAX_BUS);
		else if (!strcmp(argv[i], "-a") && i + 1 < argc)
			naddrs = parse_list(argv[++i], addrs, MAX_ADDR);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			nregs = parse_list(argv[++i], (int *)regs, MAX_REG);
			/* parse_list stores ints; re-do as unsigned */
			nregs = 0;
			{
				char *dup = strdup(argv[i]), *p = dup, *tok;
				while ((tok = strsep(&p, ",")) != NULL && nregs < MAX_REG)
					if (*tok)
						regs[nregs++] = (unsigned)strtoul(tok, NULL, 0);
				free(dup);
			}
		} else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			char *eq = strchr(argv[++i], '=');
			if (eq) {
				*eq = 0;
				set_reg = (unsigned)strtoul(argv[i], NULL, 0);
				set_val = (unsigned)strtoul(eq + 1, NULL, 0);
			}
		}
	}

	/* default register set: what the vendor playing state consists of */
	if (!nregs && set_reg == 0xffffffff) {
		static const unsigned int def[] = {
			0x00000000, 0x00002014, 0x00002018, 0x0000201c,
			0x00002024, 0x00004808, 0x00004840, 0x00004c00,
			0x00004c20, 0x00004c28, 0x00004c2c, 0x00004c40,
			0x00004c44, 0x00006000, 0x00006808, 0x00006c04,
		};
		for (i = 0; i < (int)(sizeof(def) / sizeof(def[0])); i++)
			regs[nregs++] = def[i];
	}

	for (i = 0; i < nbuses; i++) {
		b = buses[i];
		snprintf(path, sizeof(path), "/dev/i2c-%d", b);
		int fd = open(path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			continue;
		}
		for (a = 0; a < naddrs; a++) {
			if (set_reg != 0xffffffff) {
				r = write_reg(fd, addrs[a], set_reg, set_val);
				printf("%d-%02x set 0x%04x = 0x%08x : %s\n",
				       b, addrs[a], set_reg, set_val,
				       r ? strerror(errno) : "ok");
				continue;
			}
			for (r = 0; r < nregs; r++) {
				unsigned int v = 0;
				if (read_reg(fd, addrs[a], regs[r], &v) < 0)
					printf("%d-%02x 0x%08x = ERR(%s)\n", b,
					       addrs[a], regs[r], strerror(errno));
				else
					printf("%d-%02x 0x%08x = 0x%08x\n", b,
					       addrs[a], regs[r], v);
			}
		}
		close(fd);
	}
	return 0;
}
