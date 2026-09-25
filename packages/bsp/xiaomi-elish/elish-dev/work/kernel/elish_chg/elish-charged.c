// SPDX-License-Identifier: GPL-2.0
/*
 * elish-charged - complete stock charging-curve daemon for Xiaomi Pad 5
 * Pro (elish) on mainline.  Implements the k81 battery profile from the
 * stock DTBO (jeita fcc/fv ranges, OCV step charging, cold step charging),
 * drives the PM8150B switch-charger path (float/FCC registers via sysfs)
 * and the FC2 PPS + dual-bq25970 direct-charge path.
 *
 * All tables copied verbatim from the stock kernel DTBO
 * (dtb_dtbo12 qcom,battery-data qcom,k81 profile + xiaomi,usbpd-pm node).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ======== stock k81 profile (DTBO verbatim) ======== */

/* jeita-fcc-ranges (temp 0.1C lower bound, uA): */
struct range_fcc { int temp_ddc_lo, temp_ddc_hi; int fcc_ua; };
static const struct range_fcc jeita_fcc[] = {
	{ -100,   0,  810000 },
	{     1, 50, 1680000 },
	{    51,100, 4200000 },
	{   101,150, 6720000 },
	{   151,480,12400000 },   /* full power band: 15.1..48.0 C */
	{   481,580, 4200000 },   /* warm */
	{    0,   0,       0 },    /* terminator */
};
#define JEITA_TOO_COLD_DDC  (-100)
#define JEITA_TOO_HOT_DDC   (580)

/* jeita-fv-ranges (temp 0.1C, uV) */
struct range_fv { int temp_ddc_lo, temp_ddc_hi; int fv_uv; };
static const struct range_fv jeita_fv[] = {
	{ -100, 150, 4450000 },
	{   151,480, 4500000 },   /* FFC float in the normal band */
	{   481,580, 4100000 },
	{    0,   0,       0 },
};

/* step-chg-ranges (OCV uV -> uA), vendor qcom,ocv-based-step-chg */
struct range_step { int v_lo_uv, v_hi_uv; int fcc_ua; };
static const struct range_step step_chg[] = {
	{ 3000000, 3349000, 1000000 },
	{ 3350000, 4199000, 12400000 },
	{ 4200000, 4449000, 10800000 },
	{ 4450000, 4500000,  6720000 },
	{       0,       0,        0 },
};

/* cold-step-chg-ranges (temp < 15.0C) */
static const struct range_step cold_step_chg[] = {
	{ 3000000, 4199000, 1000000 },
	{ 4200000, 4450000,  660000 },
	{       0,       0,        0 },
};

/* thermal-fcc-pps-bq ladder (stock, uA) - applied progressively as the
 * battery heats past 43C; index 0 is unthrottled */
static const int thermal_fcc_pps_bq[] = {
	12400000, 10000000, 8800000, 8000000, 7000000, 6000000, 5600000,
	5000000, 4000000, 3000000, 2500000, 2000000, 1400000, 1000000,
	700000, 300000
};
#define THERM_FCC_LEVELS 16
#define THERM_START_DDC 430   /* 43.0C: begin stepping down */
#define THERM_STEP_DDC   10    /* one ladder level per 1.0C */

/* thermal-mitigation-pd-base (ICL ladder for the SW path, uA) */
static const int thermal_icl_pd[] = {
	3000000, 2800000, 2600000, 2400000, 2200000, 2000000, 1800000,
	1600000, 1600000, 1400000, 1200000, 1100000, 1050000, 1000000,
	950000, 500000
};

/* ======== xiaomi,usbpd-pm node (DTBO verbatim) ======== */
#define PD_BAT_VOLT_MAX_MV        4480   /* FFC target */
#define PD_BAT_VOLT_MAX_NONFFC_MV 4450
#define PD_BAT_CURR_MAX_MA        12400
#define PD_BUS_VOLT_MAX_MV        12000
#define PD_BUS_CURR_MAX_MA        6200
#define PD_BUS_CURR_COMP_MA       50
#define CELL_VOL_HIGH_MV          4450
#define CELL_VOL_MAX_MV           4487
#define STEP_CHG_HIGH_CURR_MA    6720
#define FCC_MAX_MASTER_ONLY_MA   6000
#define CAP_HIGH_THR              80     /* % below which dual pump allowed */
#define CAP_TOO_HIGH_THR          95
#define MIN_VBAT_FOR_CP_MV        3500
#define START_DC_FCC_MIN_MA       2000
#define TAPER_DONE_NORMAL_MA      2200
#define TAPER_STEP_MA             200
#define BQ_TAPER_HYS_MV           50
#define CELL_HIGH_COUNT_MAX       2
#define OVER_CELL_MAX_COUNT       2
#define BUS_VOLT_INIT_UP_MV       400
#define STEP_MV                    20
#define FC2_STEP_MA                50
#define FC2_STEPS                   1    /* 原厂 pm_config.fc2_steps */
#define HIGH_IBUS_LIMI_THR_MA    4000    /* 原厂 HIGH_IBUS_LIMI_THR_MA */
#define IBUS_TARGET_COMP_MA       100    /* 原厂 IBUS_TARGET_COMP_MA */
#define SLAVE_SETTLE_TICKS         12    /* 从泵使能后静置 ~6s 再判 ibus（实测爬坡需 4s+）*/
#define LOOP_MS                    500
#define PUMP_INVALID              (-999999)
#define IBUS_SLAVE_OFF_MA          450
#define VBUS_TUNE_MAX              80

/* ---- 泵芯片结温降额（tdie_raw，℃ 整数）----
 * 实测：空载 44~50℃，双泵满载（~34W 电池功率）时 54~60℃。
 * 芯片自身的热调节阈值在 DT 里是 145℃(master)/125℃(slave)，那只是最后一道
 * 硬件保护；这里在软件侧提前收电流，避免泵长期贴着高温跑。
 * 降额只作用在本轮的 fcc 上（不改 eff_fcc_ma），所以结温降下来后能自动恢复。 */
#define TDIE_DERATE_START_C        65   /* 从这颗结温开始降流 */
#define TDIE_DERATE_STEP_MA       300   /* 每高 1℃ 降 300mA */
#define TDIE_DERATE_FLOOR_MA     3000   /* 降额下限 */
#define TDIE_HARD_STOP_C           80   /* 超过就退出 FC2，交回 SW 路径 */

enum {
	ST_CHECK,
	ST_FC2_ENTRY_1,
	ST_FC2_ENTRY_2,
	ST_FC2_ENTRY_3,
	ST_FC2_TUNE,
	ST_FC2_EXIT,
};

static volatile int running = 1;
static int dbg = 1;
#define logd(...) do { if (dbg) { printf("[chg] " __VA_ARGS__); fflush(stdout);} } while (0)

static void on_signal(int s) { (void)s; running = 0; }

/* ======== sysfs helpers ======== */
static long rd_int(const char *path)
{
	char buf[64];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return LONG_MIN;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return LONG_MIN;
	buf[n] = '\0';
	return strtol(buf, NULL, 10);
}

static int wr(const char *path, long v)
{
	static int last_wr_errno;
	char buf[64];
	int fd, len, ret;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%ld", v);
	ret = (int)write(fd, buf, len);
	if (ret != len && errno != last_wr_errno) {
		logd("wr %s=%ld: %s\n", path, v, strerror(errno));
		last_wr_errno = errno;
	}
	close(fd);
	return ret == len ? 0 : -1;
}

/* ======== paths ======== */
static char tcpm_psy[256], sw_charger[256];
static char pump_master[256], pump_slave[256];
static char gauge0[256], gauge1[256];

static int find_paths(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	int pumps = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, "tcpm-source-psy-", 16))
			snprintf(tcpm_psy, sizeof(tcpm_psy),
				 "/sys/class/power_supply/%s", e->d_name);
		else if (!strcmp(e->d_name, "pm8150b-charger"))
			snprintf(sw_charger, sizeof(sw_charger),
				 "/sys/class/power_supply/%s", e->d_name);
	}
	closedir(d);
	if (!tcpm_psy[0] || !sw_charger[0])
		return -1;

	snprintf(gauge0, sizeof(gauge0), "/sys/class/power_supply/bq27z561-0");
	snprintf(gauge1, sizeof(gauge1), "/sys/class/power_supply/bq27z561-1");

	d = opendir("/sys/bus/i2c/devices");
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char *dash = strrchr(e->d_name, '-');
		char buf[512];

		if (!dash)
			continue;
		snprintf(buf, sizeof(buf),
			 "/sys/bus/i2c/devices/%s/charge_enabled", e->d_name);
		if (!strcmp(dash, "-0065") && access(buf, F_OK) == 0) {
			snprintf(pump_master, sizeof(pump_master),
				 "/sys/bus/i2c/devices/%s", e->d_name);
			pumps++;
		} else if (!strcmp(dash, "-0066") && access(buf, F_OK) == 0) {
			snprintf(pump_slave, sizeof(pump_slave),
				 "/sys/bus/i2c/devices/%s", e->d_name);
			pumps++;
		}
	}
	closedir(d);
	logd("tcpm=%s sw=%s pumps=%d master=%s slave=%s\n",
	     tcpm_psy, sw_charger, pumps, pump_master, pump_slave);
	return pumps >= 1 ? 0 : -1;
}

/* ======== battery ======== */
static int vcell0_mv, vcell1_mv, vbat_mv, ibat_ma, cap_pct, temp_ddc;

static int battery_read(void)
{
	long v0, v1, i0, i1, t, c;
	char p[512];

	snprintf(p, sizeof(p), "%s/voltage_now", gauge0);
	v0 = rd_int(p);
	snprintf(p, sizeof(p), "%s/voltage_now", gauge1);
	v1 = rd_int(p);
	snprintf(p, sizeof(p), "%s/current_now", gauge0);
	i0 = rd_int(p);
	snprintf(p, sizeof(p), "%s/current_now", gauge1);
	i1 = rd_int(p);
	snprintf(p, sizeof(p), "%s/temp", gauge0);
	t = rd_int(p);
	snprintf(p, sizeof(p), "%s/temp", gauge1);
	c = rd_int(p);
	if (t == LONG_MIN)
		c = t;   /* keep one invalid check */
	snprintf(p, sizeof(p), "%s/capacity", gauge0);
	c = rd_int(p);
	if (v0 == LONG_MIN || v1 == LONG_MIN || i0 == LONG_MIN ||
	    i1 == LONG_MIN || t == LONG_MIN || c == LONG_MIN)
		return -1;

	vcell0_mv = v0 / 1000;
	vcell1_mv = v1 / 1000;
	vbat_mv = vcell0_mv > vcell1_mv ? vcell0_mv : vcell1_mv;
	ibat_ma = (int)(i0 / 1000) + (int)(i1 / 1000);
	temp_ddc = (int)t;
	cap_pct = (int)c;
	return 0;
}

/* ======== profile evaluation ======== */

/* 阶梯表查值：半开区间 [lo, hi)。缝隙/上界回退到"最后越过的区间"
 * （保守：缝隙里沿用更低电压段的限流）。低于首段时用首段限流。 */
static int step_lookup(const struct range_step *tbl, int uv)
{
	const struct range_step *st;
	int last = -1;

	for (st = tbl; st->v_hi_uv; st++) {
		if (uv >= st->v_lo_uv && uv < st->v_hi_uv)
			return st->fcc_ua;
		if (uv >= st->v_hi_uv)
			last = st->fcc_ua;
	}
	return last > 0 ? last : tbl[0].fcc_ua;
}

static int profile_fcc_ua(void)
{
	int fcc = PD_BAT_CURR_MAX_MA * 1000;
	int cell_max_uv = vbat_mv * 1000;
	int step_ua;

	/* jeita fcc by temperature.
	 * 注意：不能用 hi==0 当终止符——首段区间 hi=0.0C，
	 * 用 lo==0&&hi==0 识别表尾哨兵。 */
	{
		const struct range_fcc *j;

		for (j = jeita_fcc; !(j->temp_ddc_lo == 0 && j->temp_ddc_hi == 0);
		     j++) {
			if (temp_ddc >= j->temp_ddc_lo &&
			    temp_ddc <= j->temp_ddc_hi) {
				fcc = j->fcc_ua;
				break;
			}
		}
	}
	if (temp_ddc < JEITA_TOO_COLD_DDC || temp_ddc > JEITA_TOO_HOT_DDC)
		return 0;   /* no charging outside the jeita window */

	/* step charging (cold table below 15.0C) */
	step_ua = step_lookup(temp_ddc < 150 ? cold_step_chg : step_chg,
			      cell_max_uv);
	if (step_ua < fcc)
		fcc = step_ua;

	/* thermal derating ladder (battery temp) */
	if (temp_ddc >= THERM_START_DDC) {
		int level = (temp_ddc - THERM_START_DDC) / THERM_STEP_DDC;

		if (level >= THERM_FCC_LEVELS)
			level = THERM_FCC_LEVELS - 1;
		if (thermal_fcc_pps_bq[level] < fcc)
			fcc = thermal_fcc_pps_bq[level];
	}
	return fcc;
}

static int profile_fv_mv(void)
{
	const struct range_fv *j;

	for (j = jeita_fv; !(j->temp_ddc_lo == 0 && j->temp_ddc_hi == 0); j++) {
		if (temp_ddc >= j->temp_ddc_lo && temp_ddc <= j->temp_ddc_hi)
			return j->fv_uv / 1000;
	}
	return 4100;   /* outside any band: safest low float */
}

static int profile_sw_icl_ua(void)
{
	int level = 0, icl;

	if (temp_ddc >= THERM_START_DDC) {
		level = (temp_ddc - THERM_START_DDC) / THERM_STEP_DDC;
		if (level >= THERM_FCC_LEVELS)
			level = THERM_FCC_LEVELS - 1;
	}
	icl = thermal_icl_pd[level];
	/* cold: stock dcp/qc tables cap lower; mirror with the same ladder */
	return icl;
}

/* ======== SW (switch) charger path control ======== */
static int sw_fcc_ma;   /* what we last wrote */
static int sw_fv_mv;

static void sw_apply(int fcc_ma, int fv_mv)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/charge_current", sw_charger);
	if (wr(p, fcc_ma) == 0)
		sw_fcc_ma = fcc_ma;
	snprintf(p, sizeof(p), "%s/float_voltage", sw_charger);
	if (wr(p, fv_mv) == 0)
		sw_fv_mv = fv_mv;
}

static void sw_enable(bool on)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/charge_enabled", sw_charger);
	wr(p, on ? 1 : 0);
}

/* ======== pump ======== */
static int p_vbus, p_vbat, p_ibus, p2_ibus;
static int p_tdie_m, p_tdie_s;  /* 泵芯片结温（℃，读不到为 PUMP_INVALID） */
static int p_vbat_reg;          /* 泵硬件进入 vbat 调节（驱动新增属性；读不到按 0 处理） */
static bool master_on, slave_on;

static void pump_read(void)
{
	char p[512];

	p_vbus = p_vbat = p_ibus = PUMP_INVALID;
	p2_ibus = PUMP_INVALID;
	p_tdie_m = p_tdie_s = PUMP_INVALID;
	p_vbat_reg = 0;
	if (pump_master[0]) {
		snprintf(p, sizeof(p), "%s/vbus_mv", pump_master);
		p_vbus = (int)rd_int(p);
		snprintf(p, sizeof(p), "%s/vbat_mv", pump_master);
		p_vbat = (int)rd_int(p);
		snprintf(p, sizeof(p), "%s/ibus_ma", pump_master);
		p_ibus = (int)rd_int(p);
		snprintf(p, sizeof(p), "%s/vbat_reg", pump_master);
		if ((int)rd_int(p) > 0)
			p_vbat_reg = 1;
		snprintf(p, sizeof(p), "%s/tdie_raw", pump_master);
		p_tdie_m = (int)rd_int(p);
	}
	if (pump_slave[0]) {
		snprintf(p, sizeof(p), "%s/ibus_ma", pump_slave);
		p2_ibus = (int)rd_int(p);
		snprintf(p, sizeof(p), "%s/tdie_raw", pump_slave);
		p_tdie_s = (int)rd_int(p);
	}
}

static void pump_set(const char *base, bool on)
{
	char p[512];

	if (!base[0])
		return;
	snprintf(p, sizeof(p), "%s/charge_enabled", base);
	wr(p, on ? 1 : 0);
}

/* ======== PPS ======== */
static int tcpm_online;

static void tcpm_read(void)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/online", tcpm_psy);
	tcpm_online = (int)rd_int(p);
}

static int pps_activate(bool on)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/online", tcpm_psy);
	return wr(p, on ? 2 : 1);
}

/*
 * PPS 激活之后，tcpm psy 的 voltage_min/voltage_max/current_max 才反映对端
 * APDO 的真实能力（内核 tcpm_psy_get_voltage_min/max、get_current_max 在
 * pps_data.active 时返回 APDO 的值）。
 *
 * 硬编码的 PD_BUS_VOLT_MAX_MV / PD_BUS_CURR_MAX_MA（12000mV / 6200mA）来自原厂
 * DT 的 qcom,usb-icl-ua 等参数，并不等于 APDO 的能力。请求一旦超过 APDO 上限，
 * 内核 tcpm_psy_set_prop() 的
 *     if (val->intval > port->pps_data.max_curr * 1000) ret = -EINVAL;
 * 会直接拒绝，整个 FC2 进入流程随即失败、退避 60 秒后重试 —— 实测每轮都失败
 * （日志里 "pps activate failed"，且 pps on 从未出现）。
 */
static int lim_vmin = -1, lim_vmax = -1, lim_imax = -1;

static void tcpm_limits(int *vmin_mv, int *vmax_mv, int *imax_ma)
{
	char p[512];
	long v;

	*vmin_mv = 0;
	*vmax_mv = PD_BUS_VOLT_MAX_MV;
	*imax_ma = PD_BUS_CURR_MAX_MA;

	snprintf(p, sizeof(p), "%s/voltage_min", tcpm_psy);
	v = rd_int(p);
	if (v > 0 && v / 1000 < *vmax_mv)
		*vmin_mv = (int)(v / 1000);

	snprintf(p, sizeof(p), "%s/voltage_max", tcpm_psy);
	v = rd_int(p);
	if (v > 0 && v / 1000 < *vmax_mv)
		*vmax_mv = (int)(v / 1000);

	snprintf(p, sizeof(p), "%s/current_max", tcpm_psy);
	v = rd_int(p);
	if (v > 0 && v / 1000 < *imax_ma)
		*imax_ma = (int)(v / 1000);

	if (*vmin_mv != lim_vmin || *vmax_mv != lim_vmax || *imax_ma != lim_imax) {
		lim_vmin = *vmin_mv;
		lim_vmax = *vmax_mv;
		lim_imax = *imax_ma;
		logd("tcpm APDO: %d..%d mV, max %d mA\n",
		     *vmin_mv, *vmax_mv, *imax_ma);
	}
}

/* 按 APDO 实际范围钳制；调用者传指针，回来即为真正生效的请求值 */
static int pps_request(int *mv, int *ma)
{
	char p[512];
	int vmin, vmax, imax;

	tcpm_limits(&vmin, &vmax, &imax);
	if (*mv > vmax)
		*mv = vmax;
	if (*mv < vmin)
		*mv = vmin;
	if (*ma > imax)
		*ma = imax;
	if (*ma < 0)
		*ma = 0;

	snprintf(p, sizeof(p), "%s/voltage_now", tcpm_psy);
	if (wr(p, (long)*mv * 1000))
		return -1;
	snprintf(p, sizeof(p), "%s/current_now", tcpm_psy);
	if (wr(p, (long)*ma * 1000))
		return -1;
	return 0;
}

/* ======== FC2 state ======== */
static int req_v_mv, req_i_ma;
static int eff_fcc_ma = PD_BAT_CURR_MAX_MA;
static int no_need_slave;
static int cell_high_cnt, over_cell_cnt, tune_retry, slave_low_cnt;
static int pps_backoff;

static void fc2_teardown(bool restore_sw)
{
	pump_set(pump_master, false);
	pump_set(pump_slave, false);
	master_on = slave_on = false;
	pps_activate(false);
	if (restore_sw)
		sw_enable(true);
}

static int fc2_tune(int *next, int profile_fcc_ma)
{
	int fcc;
	int ibus_limit;
	int cell_max = vcell0_mv > vcell1_mv ? vcell0_mv : vcell1_mv;
	int new_v, new_i;

	/* 曲线表钳制：电池升温/电压区间变化时 FCC 跟随下降（只降不升，
	 * 上升由 taper 退出后重新进入 FC2 完成） */
	if (eff_fcc_ma > profile_fcc_ma)
		eff_fcc_ma = profile_fcc_ma;
	fcc = eff_fcc_ma;

	if (no_need_slave && fcc > FCC_MAX_MASTER_ONLY_MA)
		fcc = FCC_MAX_MASTER_ONLY_MA;

	/* ---- 泵结温降额环 ----
	 * 两颗泵取更高的结温；超过 TDIE_DERATE_START_C 后每升高 1℃ 把本轮 fcc
	 * 收 300mA（下限 TDIE_DERATE_FLOOR_MA），三环控制会跟着把 ibus 目标压下来；
	 * 到 TDIE_HARD_STOP_C 直接退出 FC2 交给 SW 路径（并退避 10s）。 */
	{
		int die_max = PUMP_INVALID;

		if (p_tdie_m != PUMP_INVALID && p_tdie_m > die_max)
			die_max = p_tdie_m;
		if (p_tdie_s != PUMP_INVALID && p_tdie_s > die_max)
			die_max = p_tdie_s;

		if (die_max != PUMP_INVALID && die_max >= TDIE_HARD_STOP_C) {
			logd("pump die %dC >= %dC, exit fc2\n", die_max,
			     TDIE_HARD_STOP_C);
			*next = ST_FC2_EXIT;
			pps_backoff = 20;   /* 10s 内不再进 FC2 */
			return 0;
		}
		if (die_max != PUMP_INVALID && die_max >= TDIE_DERATE_START_C) {
			int cap = PD_BAT_CURR_MAX_MA -
				  (die_max - TDIE_DERATE_START_C) * TDIE_DERATE_STEP_MA;

			if (cap < TDIE_DERATE_FLOOR_MA)
				cap = TDIE_DERATE_FLOOR_MA;
			if (cap < fcc) {
				logd("pump die %dC, fcc %d -> %d\n", die_max, fcc, cap);
				fcc = cap;
			}
		}
	}

	ibus_limit = fcc / 2 + PD_BUS_CURR_COMP_MA;
	if (ibus_limit > PD_BUS_CURR_MAX_MA)
		ibus_limit = PD_BUS_CURR_MAX_MA;

	/* cell voltage protections (stock) */
	if (cell_max > CELL_VOL_MAX_MV) {
		if (over_cell_cnt++ > OVER_CELL_MAX_COUNT) {
			over_cell_cnt = 0;
			eff_fcc_ma -= TAPER_STEP_MA;
			logd("cell>max, fcc->%d\n", eff_fcc_ma);
		}
	} else {
		over_cell_cnt = 0;
	}
	if (cell_max > CELL_VOL_HIGH_MV) {
		if (cell_high_cnt++ > CELL_HIGH_COUNT_MAX) {
			cell_high_cnt = 0;
			eff_fcc_ma = STEP_CHG_HIGH_CURR_MA;
			logd("cell high, fcc->%d\n", eff_fcc_ma);
		}
	} else {
		cell_high_cnt = 0;
	}

	if (eff_fcc_ma < TAPER_DONE_NORMAL_MA) {
		logd("taper done\n");
		*next = ST_FC2_EXIT;
		return 0;
	}

	/* 从泵判定：必须等它稳定后再看 ibus。
	 * 原实现使能后一个循环（0.5s）就读，泵还没爬坡，常被判成"没电流"而关掉。 */
	if (slave_on && p2_ibus != PUMP_INVALID) {
		if (p2_ibus < IBUS_SLAVE_OFF_MA) {
			if (++slave_low_cnt >= SLAVE_SETTLE_TICKS) {
				pump_set(pump_slave, false);
				slave_on = false;
				no_need_slave = 1;
				logd("slave ibus %d low, single pump\n", p2_ibus);
			}
		} else {
			slave_low_cnt = 0;
		}
	}

	/* ================= 原厂三环控制（本次修复的核心）=================
	 * 参照 stock drivers/power/supply/ti/pd_policy_manager.c：
	 *   step_vbat / step_ibus / step_ibat 三者取 min，再叠加硬件调节状态
	 *   (vbat_reg) 与 bus alarm，得到最终步进，累积到请求电压上。
	 *
	 * 原实现只按 vbat*2 + 固定偏置算电压（纯电压环、且每轮重算），
	 * 电流环的"没到目标就继续抬压"这一半完全缺失，结果泵恒定停在
	 * 电压环的最低点：实测泵输入仅 ~0.5A、电池 ~1.3A，远低于 fcc。
	 */
	{
		int apdo_vmin = 0, apdo_vmax = PD_BUS_VOLT_MAX_MV;
		int apdo_imax = PD_BUS_CURR_MAX_MA;
		int v_limit = profile_fv_mv();      /* 按温度档的 CV 限压 */
		int step_vbat, step_ibus, step_ibat, steps;
		int ibus_total = (p_ibus == PUMP_INVALID) ? 0 : p_ibus;

		if (slave_on && p2_ibus != PUMP_INVALID)
			ibus_total += p2_ibus;

		tcpm_limits(&apdo_vmin, &apdo_vmax, &apdo_imax);
		if (ibus_limit > apdo_imax)
			ibus_limit = apdo_imax;
		if (ibus_limit >= HIGH_IBUS_LIMI_THR_MA)
			ibus_limit += IBUS_TARGET_COMP_MA;

		/* vbat 环：到 CV 点就收 */
		if (cell_max > v_limit)
			step_vbat = -FC2_STEPS;
		else if (cell_max < v_limit - 10)
			step_vbat = FC2_STEPS;
		else
			step_vbat = 0;

		/* ibat 环：电池电流没到 fcc 就继续抬压 */
		if (ibat_ma < fcc)
			step_ibat = FC2_STEPS;
		else if (ibat_ma > fcc + 50)
			step_ibat = -FC2_STEPS;
		else
			step_ibat = 0;

		/* ibus 环：泵输入电流没到目标就继续抬压 */
		if (ibus_total < ibus_limit - 50)
			step_ibus = FC2_STEPS;
		else if (ibus_total > ibus_limit)
			step_ibus = -FC2_STEPS;
		else
			step_ibus = 0;

		steps = step_vbat < step_ibus ? step_vbat : step_ibus;
		steps = steps < step_ibat ? steps : step_ibat;

		/* 硬件已进入 vbat 调节（泵自身在钳压）：强制降压，原厂 3 倍步长 */
		if (p_vbat_reg && steps > -3 * FC2_STEPS)
			steps = -3 * FC2_STEPS;

		new_v = req_v_mv + steps * STEP_MV;
		if (new_v < vbat_mv * 2)        /* 不能低于 2×VBAT，否则泵饿死 */
			new_v = vbat_mv * 2;
		if (new_v > apdo_vmax)
			new_v = apdo_vmax;
		new_i = ibus_limit;

		if (pps_request(&new_v, &new_i) == 0) {
			req_v_mv = new_v;
			req_i_ma = new_i;
		}
		logd("loops vbat=%+d ibus=%+d ibat=%+d steps=%+d req_v=%d vlim=%d "
		     "ibus_tot=%d fcc=%d die=%d/%d\n", step_vbat, step_ibus,
		     step_ibat, steps, req_v_mv, v_limit, ibus_total, fcc,
		     p_tdie_m, p_tdie_s);
	}

	logd("tune vbat=%d ibat=%d fcc=%d ibus_l=%d vbus=%d req v=%d i=%d "
	     "cell=%d t=%d\n", vbat_mv, ibat_ma, eff_fcc_ma, ibus_limit,
	     p_vbus, req_v_mv, req_i_ma, cell_max, temp_ddc);
	return 0;
}

int main(int argc, char **argv)
{
	int state = ST_CHECK;
	int n;

	if (argc > 1 && !strcmp(argv[1], "-q"))
		dbg = 0;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (find_paths()) {
		fprintf(stderr, "charged: missing paths (pumps/tcpm/charger)\n");
		return 1;
	}

	pump_set(pump_master, false);
	pump_set(pump_slave, false);

	logd("elish-charged start\n");
	while (running) {
		if (battery_read())
			goto sleep;
		tcpm_read();
		pump_read();

		/* ---- global safety gate: jeita window ---- */
		if (temp_ddc > JEITA_TOO_HOT_DDC ||
		    temp_ddc < JEITA_TOO_COLD_DDC) {
			if (master_on || slave_on)
				fc2_teardown(false);  /* 不恢复 SW：温度停机 */
			sw_enable(false);
			logd("jeita stop: t=%d\n", temp_ddc);
			goto sleep;
		}
		/* 泵运行期间开关充电必须保持关闭（原厂 fc2_disable_sw）；
		 * 只有泵全关时才恢复开关充电。 */
		if (!master_on && !slave_on)
			sw_enable(true);

		/* ---- curve-limited FCC/FV for the SW path (always) ---- */
		n = profile_fcc_ua() / 1000;
		if (n > PD_BAT_CURR_MAX_MA)
			n = PD_BAT_CURR_MAX_MA;
		{
			int fv = profile_fv_mv();
			int icl_ma = profile_sw_icl_ua() / 1000;

			if (n != sw_fcc_ma || fv != sw_fv_mv)
				sw_apply(n, fv);
			/* also steer the input limit for the SW path */
			{
				char p[512];

				snprintf(p, sizeof(p), "%s/current_max",
					 sw_charger);
				wr(p, icl_ma);
			}
		}

		/* ---- FC2 state machine ---- */
		switch (state) {
		case ST_CHECK:
			eff_fcc_ma = n;   /* reset taper each cycle in idle */
			if (pps_backoff > 0)
				pps_backoff--;
			if (!master_on && !slave_on && pps_backoff == 0) {
				if (tcpm_online < 1)
					break;
				if (vbat_mv < MIN_VBAT_FOR_CP_MV)
					break;
				if (vbat_mv > PD_BAT_VOLT_MAX_MV - BQ_TAPER_HYS_MV ||
				    cap_pct >= CAP_TOO_HIGH_THR)
					break;
				if (temp_ddc < 151 || temp_ddc >= 480)
					break;   /* pumps only 15.1..47.9C */
				if (n < START_DC_FCC_MIN_MA)
					break;
				logd("entry: vbat=%d cap=%d t=%d fcc=%d\n",
				     vbat_mv, cap_pct, temp_ddc, n);
				state = ST_FC2_ENTRY_1;
			}
			break;

		case ST_FC2_ENTRY_1:
			if (tcpm_online < 1) {
				state = ST_FC2_EXIT;
				break;
			}
			req_v_mv = vbat_mv * 2 + BUS_VOLT_INIT_UP_MV;
			req_i_ma = PD_BUS_CURR_MAX_MA;
			if (pps_activate(true) == 0 &&
			    pps_request(&req_v_mv, &req_i_ma) == 0) {
				logd("pps on v=%d i=%d\n", req_v_mv, req_i_ma);
				state = ST_FC2_ENTRY_2;
				tune_retry = 0;
			} else {
				logd("pps activate failed, backoff 60s\n");
				pps_backoff = 120;   /* 120 x 500ms */
				state = ST_FC2_EXIT;
			}
			break;

		case ST_FC2_ENTRY_2:
			if (tcpm_online < 1) {
				state = ST_FC2_EXIT;
				break;
			}
			if (p_vbus < vbat_mv * 2 + BUS_VOLT_INIT_UP_MV - 50) {
				tune_retry++;
				req_v_mv += STEP_MV;
				pps_request(&req_v_mv, &req_i_ma);
			} else if (p_vbus > vbat_mv * 2 + BUS_VOLT_INIT_UP_MV + 200) {
				tune_retry++;
				req_v_mv -= STEP_MV;
				pps_request(&req_v_mv, &req_i_ma);
			} else {
				logd("vbus tuned (%d retries)\n", tune_retry);
				state = ST_FC2_ENTRY_3;
				break;
			}
			if (tune_retry > VBUS_TUNE_MAX) {
				logd("tune failed\n");
				state = ST_FC2_EXIT;
			}
			break;

		case ST_FC2_ENTRY_3:
			sw_enable(false);   /* stock fc2_disable_sw */
			/* 原厂顺序：先 master 后 slave（usbpd_pm_enable_cp → _cp_sec）。
			 * 实测从泵必须在主泵已经工作之后才会爬坡：先开从泵时它恒为
			 * 6~7mA（然后被判死），先开主泵后从泵 4s 内就能到 1.2A。 */
			if (!master_on) {
				pump_set(pump_master, true);
				master_on = true;
				usleep(200000);
			}
			if (!slave_on && pump_slave[0] &&
			    cap_pct < CAP_HIGH_THR && temp_ddc < 460) {
				pump_set(pump_slave, true);
				slave_on = true;
			}
			no_need_slave = slave_on ? 0 : 1;
			if (master_on) {
				logd("pumps on m=%d s=%d\n", master_on, slave_on);
				state = ST_FC2_TUNE;
			} else {
				state = ST_FC2_EXIT;
			}
			break;

		case ST_FC2_TUNE:
			{
				int next = state;

				if (tcpm_online < 1 || temp_ddc >= 480 ||
				    temp_ddc < 151 ||
				    vbat_mv > PD_BAT_VOLT_MAX_MV - BQ_TAPER_HYS_MV ||
				    cap_pct >= CAP_TOO_HIGH_THR)
					next = ST_FC2_EXIT;
				else
					fc2_tune(&next, n);
				if (next != state)
					state = next;
			}
			break;

		case ST_FC2_EXIT:
			fc2_teardown(true);
			logd("fc2 exit\n");
			state = ST_CHECK;
			eff_fcc_ma = PD_BAT_CURR_MAX_MA;
			no_need_slave = 0;
			break;
		}
sleep:
		usleep(LOOP_MS * 1000);
	}

	fc2_teardown(true);
	return 0;
}
