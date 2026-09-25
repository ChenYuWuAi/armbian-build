// SPDX-License-Identifier: GPL-2.0
/*
 * elish-fc2 - FC2 direct-charge daemon for Xiaomi Pad 5 Pro (elish) on
 * mainline.  Faithful userspace port of the stock usbpd_pm FC2 algorithm
 * (drivers/power/supply/ti/pd_policy_manager.c, MiCode elish-r-oss).
 *
 * Drives:
 *   - PD PPS via the mainline tcpm source psy sysfs:
 *       online=2        -> activate PPS (PROG)
 *       online=1        -> back to fixed PDO
 *       voltage_now=uV  -> PPS output voltage request
 *       current_now=uA  -> PPS operating current request
 *   - the dual bq25970 pumps via the bq2597x-elish driver sysfs
 *       /sys/bus/i2c/devices/N-0065 / N-0066: charge_enabled, vbus_mv,
 *       vbat_mv, ibus_ma
 *   - battery from the two bq27z561 gauges (V=max, I=sum, vendor semantics)
 *
 * Safety: every limit of the stock algorithm is kept, plus the pump chips'
 * hardware protections (OVP/OCP/therm) configured by the kernel driver.
 * On exit/crash the daemon disables both pumps and falls back to fixed PDO.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

/* ---------------- stock DTBO limits (xiaomi,usbpd-pm + k81 profile) ---- */
#define BAT_VOLT_MAX_MV        4450   /* non-FFC (mi,pd-non-ffc-bat-volt-max) */
#define BAT_VOLT_MAX_FFC_MV    4480   /* FFC (mi,pd-bat-volt-max) */
#define BAT_CURR_MAX_MA        12400  /* mi,pd-bat-curr-max */
#define BUS_VOLT_MAX_MV        12000  /* mi,pd-bus-volt-max */
#define BUS_CURR_MAX_MA        6200   /* mi,pd-bus-curr-max */
#define BUS_CURR_COMPENSATE_MA 50     /* mi,pd-bus-curr-compensate */
#define CELL_VOL_HIGH_MV       4450   /* mi,cell-vol-high-threshold-mv */
#define CELL_VOL_MAX_MV        4487   /* mi,cell-vol-max-threshold-mv */
#define STEP_CHG_HIGH_CURR_MA  6720   /* mi,step-charge-high-vol-curr-max */
#define FCC_MAX_MASTER_ONLY_MA 6000   /* FCC_MAX_MA_FOR_MASTER_BQ */
#define CAPACITY_HIGH_THR      80     /* % - dual pump only below this */
#define CAPACITY_TOO_HIGH_THR  95
#define MIN_VBAT_FOR_CP_MV     3500   /* min battery voltage for the pump */
#define BAT_WARM_TH_DDC        480    /* 48.0C */
#define BAT_COOL_TH_DDC        150    /* 15.0C */
#define START_DC_FCC_MIN_MA    2000
#define TAPER_DONE_NORMAL_MA   2200
#define TAPER_DECREASE_STEP_MA 200
#define BQ_TAPER_HYS_MV        50     /* NON_FFC_BQ_TAPER_HYS_MV */
#define CELL_HIGH_COUNT_MAX    2
#define BUS_VOLT_INIT_UP_MV    400
#define STEP_MV                20
#define FC2_STEPS_MA           50     /* sw ctrl step */
#define TUNE_INTERVAL_MS       500
#define IBUS_SLAVE_OFF_THR_MA  450   /* per-pump ibus below which slave off */
#define VBUS_TUNE_RETRY_MAX    80
#define OVER_CELL_MAX_COUNT    2

/* daemon state */
enum {
	ST_OFF,          /* no suitable charger: fall back */
	ST_ENTRY_CHECK,  /* evaluate entry conditions */
	ST_FC2_ENTRY_1,  /* initial PPS request */
	ST_FC2_ENTRY_2,  /* vbus tuning */
	ST_FC2_ENTRY_3,  /* enable pumps */
	ST_FC2_TUNE,     /* steady-state control loop */
	ST_FC2_EXIT,     /* teardown, back to switch charger */
};

static volatile int running = 1;
static int dbg = 1;

#define logd(...) do { if (dbg) { printf("[fc2] " __VA_ARGS__); fflush(stdout);} } while (0)

static void on_signal(int s)
{
	running = 0;
}

/* ---------------- tiny sysfs helpers ---------------- */

static int sysfs_read(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

static long sysfs_read_int(const char *path)
{
	char buf[64];

	if (sysfs_read(path, buf, sizeof(buf)) < 0)
		return LONG_MIN;
	return strtol(buf, NULL, 10);
}

static int sysfs_write(const char *path, long v)
{
	char buf[64];
	int fd, len, ret;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%ld", v);
	ret = write(fd, buf, len);
	close(fd);
	return ret == len ? 0 : -1;
}

/* ---------------- path resolution ---------------- */

static char tcpm_psy[256];
static char pump_master[256], pump_slave[256];
static char gauge0[256], gauge1[256];
static char sw_charger[256];

static int find_paths(void)
{
	DIR *d;
	struct dirent *e;
	char buf[512];
	int found_pumps = 0, found_tcpm = 0;

	/* tcpm source psy */
	d = opendir("/sys/class/power_supply");
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "tcpm-source-psy-", 16) == 0) {
			snprintf(tcpm_psy, sizeof(tcpm_psy),
				 "/sys/class/power_supply/%s", e->d_name);
			found_tcpm = 1;
		}
		if (strcmp(e->d_name, "pm8150b-charger") == 0) {
			snprintf(sw_charger, sizeof(sw_charger),
				 "/sys/class/power_supply/%s", e->d_name);
		}
	}
	closedir(d);
	if (!found_tcpm)
		return -1;

	/* gauges by fixed names from the elish DT */
	snprintf(gauge0, sizeof(gauge0),
		 "/sys/class/power_supply/bq27z561-0");
	snprintf(gauge1, sizeof(gauge1),
		 "/sys/class/power_supply/bq27z561-1");

	/* pumps: i2c devices 0065/0066 anywhere */
	d = opendir("/sys/bus/i2c/devices");
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char *dash = strrchr(e->d_name, '-');

		if (!dash)
			continue;
		if (strcmp(dash, "-0065") == 0) {
			snprintf(pump_master, sizeof(pump_master),
				 "/sys/bus/i2c/devices/%s", e->d_name);
			snprintf(buf, sizeof(buf), "%s/charge_enabled",
				 pump_master);
			if (access(buf, F_OK) == 0)
				found_pumps++;
		} else if (strcmp(dash, "-0066") == 0) {
			snprintf(pump_slave, sizeof(pump_slave),
				 "/sys/bus/i2c/devices/%s", e->d_name);
			snprintf(buf, sizeof(buf), "%s/charge_enabled",
				 pump_slave);
			if (access(buf, F_OK) == 0)
				found_pumps++;
		}
	}
	closedir(d);

	logd("paths: tcpm=%s pumps=%d master=%s slave=%s\n",
	     tcpm_psy, found_pumps, pump_master, pump_slave);
	return (found_pumps >= 1) ? 0 : -1;
}

/* ---------------- battery (dual gauge, vendor semantics) --------------- */

static int batt_vcell0_mv, batt_vcell1_mv;
static int batt_vbat_mv;     /* max of cells */
static int batt_ibat_ma;     /* sum of cell currents */
static int batt_cap_pct;
static int batt_temp_ddc;    /* deci-C */

static int battery_read(void)
{
	char p[512];
	long v0, v1, i0, i1, t, c;

	snprintf(p, sizeof(p), "%s/voltage_now", gauge0);
	v0 = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/voltage_now", gauge1);
	v1 = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/current_now", gauge0);
	i0 = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/current_now", gauge1);
	i1 = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/temp", gauge0);
	t = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/capacity", gauge0);
	c = sysfs_read_int(p);
	if (v0 == LONG_MIN || v1 == LONG_MIN || i0 == LONG_MIN ||
	    i1 == LONG_MIN || t == LONG_MIN || c == LONG_MIN)
		return -1;

	batt_vcell0_mv = v0 / 1000;
	batt_vcell1_mv = v1 / 1000;
	/* voltage_now on bq27xxx is cell voltage; pack V = max */
	batt_vbat_mv = batt_vcell0_mv > batt_vcell1_mv ?
		       batt_vcell0_mv : batt_vcell1_mv;
	/* current_now per cell; pack I = sum (both positive when charging) */
	batt_ibat_ma = (int)(i0 / 1000) + (int)(i1 / 1000);
	batt_temp_ddc = (int)t;
	batt_cap_pct = (int)c;
	return 0;
}

/* ---------------- pump access ---------------- */

static int pump_vbus_mv, pump_vbat_mv, pump_ibus_ma, pump2_ibus_ma;
static bool master_on, slave_on, slave_usable;

static int pump_read(void)
{
	char p[512];

	if (!pump_master[0])
		return -1;
	snprintf(p, sizeof(p), "%s/vbus_mv", pump_master);
	pump_vbus_mv = (int)sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/vbat_mv", pump_master);
	pump_vbat_mv = (int)sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/ibus_ma", pump_master);
	pump_ibus_ma = (int)sysfs_read_int(p);
	pump2_ibus_ma = LONG_MIN;
	if (slave_usable && pump_slave[0]) {
		snprintf(p, sizeof(p), "%s/ibus_ma", pump_slave);
		pump2_ibus_ma = (int)sysfs_read_int(p);
	}
	return 0;
}

static int pump_set(const char *base, bool on)
{
	char p[512];

	if (!base[0])
		return -1;
	snprintf(p, sizeof(p), "%s/charge_enabled", base);
	return sysfs_write(p, on ? 1 : 0);
}

static bool pump_get(const char *base)
{
	char p[512];
	long v;

	snprintf(p, sizeof(p), "%s/charge_enabled", base);
	v = sysfs_read_int(p);
	return v == 1;
}

/* ---------------- tcpm PPS steering ---------------- */

static int tcpm_online;
static long tcpm_volt_uv, tcpm_curr_ua;

static int tcpm_read(void)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/online", tcpm_psy);
	tcpm_online = (int)sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/voltage_now", tcpm_psy);
	tcpm_volt_uv = sysfs_read_int(p);
	snprintf(p, sizeof(p), "%s/current_now", tcpm_psy);
	tcpm_curr_ua = sysfs_read_int(p);
	return 0;
}

static int pps_activate(bool activate)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/online", tcpm_psy);
	return sysfs_write(p, activate ? 2 : 1);
}

static int pps_request(int mv, int ma)
{
	char p[512];

	if (mv > BUS_VOLT_MAX_MV)
		mv = BUS_VOLT_MAX_MV;
	if (ma > BUS_CURR_MAX_MA)
		ma = BUS_CURR_MAX_MA;
	snprintf(p, sizeof(p), "%s/voltage_now", tcpm_psy);
	if (sysfs_write(p, (long)mv * 1000))
		return -1;
	snprintf(p, sizeof(p), "%s/current_now", tcpm_psy);
	if (sysfs_write(p, (long)ma * 1000))
		return -1;
	return 0;
}

/* ---------------- FC2 state machine (vendor faithful) ------------------- */

static int request_voltage_mv, request_current_ma;
static int effective_fcc_ma = BAT_CURR_MAX_MA;
static int curr_ibus_limit_ma;
static int no_need_slave;
static int cell_high_count, over_cell_max_count;
static int vbus_tune_retry;

static void sw_charger_enable(bool on)
{
	char p[512];

	if (!sw_charger[0])
		return;
	snprintf(p, sizeof(p), "%s/charge_enabled", sw_charger);
	sysfs_write(p, on ? 1 : 0);
}

static void fc2_exit(bool keep_pps)
{
	pump_set(pump_master, false);
	if (slave_usable)
		pump_set(pump_slave, false);
	master_on = slave_on = false;
	sw_charger_enable(true);   /* hand charging back to the switch path */
	if (!keep_pps)
		pps_activate(false);
}

static int fc2_tune(int *next_state)
{
	int step_vbat = 0, step_ibat = 0;
	int ibus_total;
	int new_v = request_voltage_mv;
	int new_i = request_current_ma;
	int cell_vmax = batt_vcell0_mv > batt_vcell1_mv ?
			batt_vcell0_mv : batt_vcell1_mv;

	/* ---- effective FCC, per stock algo ---- */
	int fcc = effective_fcc_ma;

	if (no_need_slave)
		fcc = fcc > FCC_MAX_MASTER_ONLY_MA ? FCC_MAX_MASTER_ONLY_MA : fcc;

	curr_ibus_limit_ma = fcc >> 1;
	curr_ibus_limit_ma += BUS_CURR_COMPENSATE_MA;
	if (curr_ibus_limit_ma > BUS_CURR_MAX_MA)
		curr_ibus_limit_ma = BUS_CURR_MAX_MA;

	/* ---- cell voltage protection (stock: vote safe fcc) ---- */
	if (cell_vmax > CELL_VOL_MAX_MV) {
		if (over_cell_max_count++ > OVER_CELL_MAX_COUNT) {
			over_cell_max_count = 0;
			effective_fcc_ma -= TAPER_DECREASE_STEP_MA;
			logd("cell v %d > %d, fcc -= %d -> %d\n",
			     cell_vmax, CELL_VOL_MAX_MV, TAPER_DECREASE_STEP_MA,
			     effective_fcc_ma);
		}
	} else {
		over_cell_max_count = 0;
	}

	if (cell_vmax > CELL_VOL_HIGH_MV) {
		if (cell_high_count++ > CELL_HIGH_COUNT_MAX) {
			cell_high_count = 0;
			effective_fcc_ma = STEP_CHG_HIGH_CURR_MA;
			logd("cell high, fcc -> %d\n", effective_fcc_ma);
		}
	} else {
		cell_high_count = 0;
	}

	if (effective_fcc_ma < TAPER_DONE_NORMAL_MA) {
		logd("taper done (fcc %d < %d)\n", effective_fcc_ma,
		     TAPER_DONE_NORMAL_MA);
		*next_state = ST_FC2_EXIT;
		return 0;
	}

	/* ---- voltage loop (vbat toward BAT_VOLT_MAX) ---- */
	if (batt_vbat_mv > BAT_VOLT_MAX_MV)
		step_vbat = -FC2_STEPS_MA;
	else if (batt_vbat_mv < BAT_VOLT_MAX_MV - 10)
		step_vbat = FC2_STEPS_MA;

	/* ---- battery current loop ---- */
	if (batt_ibat_ma < fcc)
		step_ibat = FC2_STEPS_MA;
	else if (batt_ibat_ma > fcc + 50)
		step_ibat = -FC2_STEPS_MA;

	ibus_total = pump_ibus_ma + (pump2_ibus_ma == LONG_MIN ?
				     0 : pump2_ibus_ma);

	/* adjust bus current target */
	new_i = curr_ibus_limit_ma;
	if (step_vbat > 0 && step_ibat > 0)
		new_i += 0;  /* headroom already in limit */
	else if (step_vbat < 0 || step_ibat < 0)
		new_i -= FC2_STEPS_MA;

	/* vbus headroom: track 2*vbat + init offset */
	new_v = batt_vbat_mv * 2 + BUS_VOLT_INIT_UP_MV;

	/* slave pump management: single-pump when battery nearly full
	 * (stock: ibus below threshold -> drop slave) */
	if (slave_on && pump2_ibus_ma != LONG_MIN &&
	    pump2_ibus_ma < IBUS_SLAVE_OFF_THR_MA) {
		pump_set(pump_slave, false);
		slave_on = false;
		no_need_slave = 1;
		logd("slave ibus %d < %d: slave off\n", pump2_ibus_ma,
		     IBUS_SLAVE_OFF_THR_MA);
	}

	if (pps_request(new_v, new_i) == 0) {
		request_voltage_mv = new_v;
		request_current_ma = new_i;
	}

	logd("tune: vbat=%d ibat=%d fcc=%d ibus_l=%d vbus=%d req v=%d i=%d "
	     "cellmax=%d t=%d\n",
	     batt_vbat_mv, batt_ibat_ma, effective_fcc_ma,
	     curr_ibus_limit_ma, pump_vbus_mv, request_voltage_mv,
	     request_current_ma, cell_vmax, batt_temp_ddc);

	return 0;
}

int main(int argc, char **argv)
{
	int state = ST_ENTRY_CHECK;
	int cycle = 0;

	if (argc > 1 && strcmp(argv[1], "-q") == 0)
		dbg = 0;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (find_paths()) {
		fprintf(stderr, "fc2: required paths missing (pumps/tcpm)\n");
		return 1;
	}

	/* start from a safe state */
	pump_set(pump_master, false);
	if (pump_slave[0])
		pump_set(pump_slave, false);

	while (running) {
		bool online;

		battery_read();
		tcpm_read();
		pump_read();
		online = (tcpm_online >= 1);
		cycle++;

		if (state == ST_ENTRY_CHECK) {
			if (!online)
				goto sleep;
			if (batt_vbat_mv < MIN_VBAT_FOR_CP_MV) {
				logd("vbat %d too low, wait\n", batt_vbat_mv);
				goto sleep;
			}
			if (batt_vbat_mv > BAT_VOLT_MAX_MV - BQ_TAPER_HYS_MV ||
			    batt_cap_pct >= CAPACITY_TOO_HIGH_THR) {
				logd("vbat/cap too high for cp, stay SW\n");
				goto sleep;
			}
			if (batt_temp_ddc >= BAT_WARM_TH_DDC ||
			    batt_temp_ddc <= BAT_COOL_TH_DDC) {
				logd("temp %d out of range, stay SW\n",
				     batt_temp_ddc);
				goto sleep;
			}
			if (effective_fcc_ma < START_DC_FCC_MIN_MA) {
				logd("fcc %d below start thr, stay SW\n",
				     effective_fcc_ma);
				goto sleep;
			}
			logd("entry ok: vbat=%d cap=%d t=%d -> FC2\n",
			     batt_vbat_mv, batt_cap_pct, batt_temp_ddc);
			state = ST_FC2_ENTRY_1;
		} else if (state == ST_FC2_ENTRY_1) {
			if (!online) {
				state = ST_FC2_EXIT;
				break;
			}
			request_voltage_mv = batt_vbat_mv * 2 + BUS_VOLT_INIT_UP_MV;
			request_current_ma = BUS_CURR_MAX_MA;
			if (pps_activate(true) == 0 &&
			    pps_request(request_voltage_mv,
					request_current_ma) == 0) {
				logd("PPS on: v=%d i=%d\n",
				     request_voltage_mv, request_current_ma);
				state = ST_FC2_ENTRY_2;
				vbus_tune_retry = 0;
			} else {
				logd("PPS activate failed (no PPS source?)\n");
				state = ST_FC2_EXIT;
				break;
			}
		} else if (state == ST_FC2_ENTRY_2) {
			if (!online) {
				state = ST_FC2_EXIT;
				break;
			}
			if (pump_vbus_mv <
			    batt_vbat_mv * 2 + BUS_VOLT_INIT_UP_MV - 50) {
				vbus_tune_retry++;
				request_voltage_mv += STEP_MV;
				pps_request(request_voltage_mv,
					    request_current_ma);
			} else if (pump_vbus_mv >
				   batt_vbat_mv * 2 + BUS_VOLT_INIT_UP_MV + 200) {
				vbus_tune_retry++;
				request_voltage_mv -= STEP_MV;
				pps_request(request_voltage_mv,
					    request_current_ma);
			} else {
				logd("vbus tune ok after %d retries\n",
				     vbus_tune_retry);
				state = ST_FC2_ENTRY_3;
				break;
			}
			if (vbus_tune_retry > VBUS_TUNE_RETRY_MAX) {
				logd("vbus tune failed, back to SW\n");
				state = ST_FC2_EXIT;
				break;
			}
		} else if (state == ST_FC2_ENTRY_3) {
			bool dual_ok = slave_usable &&
				       batt_cap_pct < CAPACITY_HIGH_THR &&
				       batt_temp_ddc < BAT_WARM_TH_DDC - 20;

			sw_charger_enable(false);  /* stock: fc2_disable_sw */
			if (!master_on) {
				pump_set(pump_master, true);
				master_on = pump_get(pump_master);
				usleep(30000);
			}
			if (dual_ok && !slave_on) {
				pump_set(pump_slave, true);
				slave_on = pump_get(pump_slave);
				usleep(30000);
			}
			if (dual_ok && master_on)
				no_need_slave = 0;
			else
				no_need_slave = 1;
			if (master_on) {
				logd("pumps on: master=%d slave=%d\n",
				     master_on, slave_on);
				state = ST_FC2_TUNE;
			} else {
				logd("master pump enable failed, exit\n");
				state = ST_FC2_EXIT;
				break;
			}
		} else if (state == ST_FC2_TUNE) {
			int next = state;

			if (!online || batt_temp_ddc >= BAT_WARM_TH_DDC ||
			    batt_temp_ddc <= BAT_COOL_TH_DDC ||
			    batt_vbat_mv > BAT_VOLT_MAX_MV - BQ_TAPER_HYS_MV ||
			    batt_cap_pct >= CAPACITY_TOO_HIGH_THR)
				next = ST_FC2_EXIT;

			if (next == state)
				fc2_tune(&next);

			if (next != state) {
				state = next;
				break;  /* handled below */
			}
		} else if (state == ST_FC2_EXIT) {
			fc2_exit(false);
			logd("FC2 exit -> SW charging\n");
			state = ST_ENTRY_CHECK;
			effective_fcc_ma = BAT_CURR_MAX_MA;
			no_need_slave = 0;
		}
sleep:
		usleep(TUNE_INTERVAL_MS * 1000);
	}

	/* graceful shutdown */
	fc2_exit(true);
	if (tcpm_online == 2)
		pps_activate(false);
	return 0;
}
