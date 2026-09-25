/* 干跑工具：在当前启动上读真实电量计 + 跑与 elish-charged 相同的曲线求值，
 * 打印将要写入的 FCC/FV/状态机决策（不写任何 sysfs）。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* 与 elish-charged.c 一致的表（复制自通过 test_curves 全边界验证的版本） */
struct range_fcc { int temp_ddc_lo, temp_ddc_hi; int fcc_ua; };
static const struct range_fcc jeita_fcc[] = {
	{ -100,   0,  810000 }, {     1, 50, 1680000 }, {    51,100, 4200000 },
	{   101,150, 6720000 }, {   151,480,12400000 }, {   481,580, 4200000 },
	{    0,   0,       0 },
};
struct range_fv { int temp_ddc_lo, temp_ddc_hi; int fv_uv; };
static const struct range_fv jeita_fv[] = {
	{ -100, 150, 4450000 }, { 151,480, 4500000 }, { 481,580, 4100000 },
	{    0,   0,       0 },
};
struct range_step { int v_lo_uv, v_hi_uv; int fcc_ua; };
static const struct range_step step_chg[] = {
	{ 3000000, 3349000, 1000000 }, { 3350000, 4199000, 12400000 },
	{ 4200000, 4449000, 10800000 }, { 4450000, 4500000,  6720000 },
	{ 0, 0, 0 },
};
static const struct range_step cold_step_chg[] = {
	{ 3000000, 4199000, 1000000 }, { 4200000, 4450000,  660000 },
	{ 0, 0, 0 },
};
static const int thermal_fcc_pps_bq[] = {
	12400000, 10000000, 8800000, 8000000, 7000000, 6000000, 5600000,
	5000000, 4000000, 3000000, 2500000, 2000000, 1400000, 1000000,
	700000, 300000
};

static long rd(const char *path)
{
	char b[64];
	FILE *f = fopen(path, "r");

	if (!f)
		return LONG_MIN;
	if (!fgets(b, sizeof(b), f)) { fclose(f); return LONG_MIN; }
	fclose(f);
	return strtol(b, NULL, 10);
}

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

int main(void)
{
	long v0 = rd("/sys/class/power_supply/bq27z561-0/voltage_now");
	long v1 = rd("/sys/class/power_supply/bq27z561-1/voltage_now");
	long i0 = rd("/sys/class/power_supply/bq27z561-0/current_now");
	long i1 = rd("/sys/class/power_supply/bq27z561-1/current_now");
	long t0 = rd("/sys/class/power_supply/bq27z561-0/temp");
	long t1 = rd("/sys/class/power_supply/bq27z561-1/temp");
	long c0 = rd("/sys/class/power_supply/bq27z561-0/capacity");
	long c1 = rd("/sys/class/power_supply/bq27z561-1/capacity");

	if (v0 == LONG_MIN || v1 == LONG_MIN) {
		printf("DRYRUN-FAIL: gauges unreadable\n");
		return 1;
	}

	int vcell0 = v0 / 1000, vcell1 = v1 / 1000;
	int vbat = vcell0 > vcell1 ? vcell0 : vcell1;
	int ibat = (int)(i0 / 1000) + (int)(i1 / 1000);
	int temp = (int)((t0 == LONG_MIN ? t1 : t0) / 1);   /* deci-C */
	int cap = (int)(c0 == LONG_MIN ? c1 : c0);

	printf("cell0=%dmV cell1=%dmV  vbat_pack=%dmV  ibat=%dmA  temp=%d.%dC  cap=%d%%\n",
	       vcell0, vcell1, vbat, ibat, temp / 10, temp % 10, cap);

	/* JEITA FCC */
	int fcc = 12400000, fv = 4100;
	const struct range_fcc *j;
	const struct range_fv *f;
	char band[32] = "none";

	for (j = jeita_fcc; !(j->temp_ddc_lo == 0 && j->temp_ddc_hi == 0); j++) {
		if (temp >= j->temp_ddc_lo && temp <= j->temp_ddc_hi) {
			fcc = j->fcc_ua;
			snprintf(band, sizeof(band), "%d..%d", j->temp_ddc_lo,
				 j->temp_ddc_hi);
			break;
		}
	}
	if (temp < -100 || temp > 580) {
		printf("jeita: t=%d OUT OF WINDOW -> STOP CHARGING\n", temp);
		return 0;
	}
	for (f = jeita_fv; !(f->temp_ddc_lo == 0 && f->temp_ddc_hi == 0); f++) {
		if (temp >= f->temp_ddc_lo && temp <= f->temp_ddc_hi) {
			fv = f->fv_uv / 1000;
			break;
		}
	}

	/* step */
	int step_ua = step_lookup(temp < 150 ? cold_step_chg : step_chg,
				  vbat * 1000);
	if (step_ua < fcc)
		fcc = step_ua;

	/* thermal ladder */
	if (temp >= 430) {
		int lvl = (temp - 430) / 10;

		if (lvl > 15)
			lvl = 15;
		if (thermal_fcc_pps_bq[lvl] < fcc)
			fcc = thermal_fcc_pps_bq[lvl];
	}

	printf("jeita band=[%s]  step=%dma\n", band, step_ua / 1000);
	printf("WOULD SET: sw_charge_current=%dma  sw_float_voltage=%dmV\n",
	       fcc / 1000, fv);
	printf("FC2 entry: %s (vbat>=%dmV && vbat<%dmV && cap<95 && "
	       "151<=t<480 && fcc>=2000)\n",
	       (vbat >= 3500 && vbat < 4430 && cap < 95 && temp >= 151 &&
		temp < 480 && fcc / 1000 >= 2000) ? "YES" : "no",
	       3500, 4430);
	printf("dual pump : %s (cap<80 && t<460)\n",
	       (cap < 80 && temp < 460) ? "allowed" : "single-only");
	printf("DRYRUN-OK\n");
	return 0;
}
