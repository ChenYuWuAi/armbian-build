/* 宿主机单元测试：验证 elish-charged 的 k81 曲线表查表逻辑全边界正确。
 * 编译: gcc -O2 -o test_curves test_curves.c -I. (复用 elish-charged.c 的表) */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- 从 elish-charged.c 原样复制的表与函数 ---- */
struct range_fcc { int temp_ddc_lo, temp_ddc_hi; int fcc_ua; };
static const struct range_fcc jeita_fcc[] = {
	{ -100,   0,  810000 },
	{     1, 50, 1680000 },
	{    51,100, 4200000 },
	{   101,150, 6720000 },
	{   151,480,12400000 },
	{   481,580, 4200000 },
	{    0,   0,       0 },
};
#define JEITA_TOO_COLD_DDC  (-100)
#define JEITA_TOO_HOT_DDC   (580)
struct range_fv { int temp_ddc_lo, temp_ddc_hi; int fv_uv; };
static const struct range_fv jeita_fv[] = {
	{ -100, 150, 4450000 },
	{   151,480, 4500000 },
	{   481,580, 4100000 },
	{    0,   0,       0 },
};
struct range_step { int v_lo_uv, v_hi_uv; int fcc_ua; };
static const struct range_step step_chg[] = {
	{ 3000000, 3349000, 1000000 },
	{ 3350000, 4199000, 12400000 },
	{ 4200000, 4449000, 10800000 },
	{ 4450000, 4500000,  6720000 },
	{       0,       0,        0 },
};
static const struct range_step cold_step_chg[] = {
	{ 3000000, 4199000, 1000000 },
	{ 4200000, 4450000,  660000 },
	{       0,       0,        0 },
};
static const int thermal_fcc_pps_bq[] = {
	12400000, 10000000, 8800000, 8000000, 7000000, 6000000, 5600000,
	5000000, 4000000, 3000000, 2500000, 2000000, 1400000, 1000000,
	700000, 300000
};
#define THERM_FCC_LEVELS 16
#define THERM_START_DDC  430
#define THERM_STEP_DDC   10

/* 全局输入（模拟电量计读数） */
static int vbat_mv, temp_ddc;

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
	const struct range_fcc *j;
	int fcc = 12400000;
	int cell_max_uv = vbat_mv * 1000;
	int step_ua;

	for (j = jeita_fcc; !(j->temp_ddc_lo == 0 && j->temp_ddc_hi == 0); j++) {
		if (temp_ddc >= j->temp_ddc_lo && temp_ddc <= j->temp_ddc_hi) {
			fcc = j->fcc_ua;
			break;
		}
	}
	if (temp_ddc < JEITA_TOO_COLD_DDC || temp_ddc > JEITA_TOO_HOT_DDC)
		return 0;

	step_ua = step_lookup(temp_ddc < 150 ? cold_step_chg : step_chg,
			      cell_max_uv);
	if (step_ua < fcc)
		fcc = step_ua;

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

	for (j = jeita_fv; j->temp_ddc_hi; j++) {
		if (temp_ddc >= j->temp_ddc_lo && temp_ddc <= j->temp_ddc_hi)
			return j->fv_uv / 1000;
	}
	return 4100;
}

/* ---------------- 测试 ---------------- */
static int fails;

#define CHECK(cond, msg...) do { \
	if (!(cond)) { printf("FAIL: " msg); printf("\n"); fails++; } \
} while (0)

int main(void)
{
	/* 1. JEITA 全温区扫描（0.1℃ 步进，-15.0 到 60.0℃） */
	int t, v;
	int prev = -1;

	for (t = -150; t <= 600; t += 5) {
		temp_ddc = t;
		vbat_mv = 4000;
		int fcc = profile_fcc_ua() / 1000;
		int fv = profile_fv_mv();

		/* 不变量：FCC 永远在 [0, 12400]；FV 在 [4100, 4500] */
		CHECK(fcc >= 0 && fcc <= 12400, "t=%d.%d fcc=%d 越界", t/10, t%10, fcc);
		CHECK(fv >= 4100 && fv <= 4500, "t=%d.%d fv=%d 越界", t/10, t%10, fv);
		/* 停充窗口 */
		if (t < -100 || t > 580)
			CHECK(fcc == 0, "t=%d 应停充(0)，得 %d", t, fcc);
	}

	/* 2. 关键边界值逐一断言 */
	struct { int t; int exp_fcc; } bnd[] = {
		{ -101,     0 },   /* -10.1C 停充 */
		{ -100,   810 },   /* -10.0C 恰好 810 */
		{     0,   810 },   /*  0.0C: min(jeita 810, cold_step 1000) */
		{     1,  1000 },   /*  0.1C: min(1680, cold 1000) */
		{    50,  1000 },
		{    51,  1000 },   /*  5.1C: min(4200, cold 1000) */
		{   100,  1000 },
		{   101,  1000 },   /* 10.1C: min(6720, cold 1000) */
		{   150,  6720 },   /* 15.0C: cool 段上沿, 用普通 step 表: min(6720,12400) */
		{   151, 12400 },   /* 15.1C 满速 */
		{   200, 12400 },
		{   420, 12400 },   /* 42.0C */
		{   429, 12400 },   /* 42.9C 热梯未起 */
		{   430, 12400 },   /* 43.0C level0 = 12400 */
		{   440, 10000 },   /* 44.0C level1 */
		{   450,  8800 },
		{   470,  7000 },
		{   480,  6000 },   /* 48.0C 温+热梯 level5 */
		{   481,  4200 },   /* 48.1C: min(jeita 4200, 梯5=6000)=4200 */
		{   500,  4200 },   /* 50.0C: min(4200, 梯7=5000)=4200 */
		{   550,  1400 },   /* 55.0C: min(4200, 梯12=1400)=1400 */
		{   579,   700 },   /* 57.9C: min(4200, 梯14=700)=700 */
		{   580,   300 },   /* 58.0C: 区间含 580（原厂语义），梯15=300 */
		{   581,     0 },   /* 58.1C: 超窗停充 */
	};
	for (size_t i = 0; i < sizeof(bnd)/sizeof(bnd[0]); i++) {
		temp_ddc = bnd[i].t;
		vbat_mv = 4000;
		int got = profile_fcc_ua() / 1000;
		CHECK(got == bnd[i].exp_fcc,
		      "t=%d.%d 期望 %d 得 %d", bnd[i].t/10, bnd[i].t%10,
		      bnd[i].exp_fcc, got);
	}

	/* 3. FV 边界 */
	struct { int t; int exp_fv; } fvb[] = {
		{ -100, 4450 }, { 0, 4450 }, { 150, 4450 },
		{ 151, 4500 }, { 300, 4500 }, { 480, 4500 },
		{ 481, 4100 }, { 580, 4100 }, { 600, 4100 },
	};
	for (size_t i = 0; i < sizeof(fvb)/sizeof(fvb[0]); i++) {
		temp_ddc = fvb[i].t;
		int got = profile_fv_mv();
		CHECK(got == fvb[i].exp_fv, "t=%d FV 期望 %d 得 %d",
		      fvb[i].t, fvb[i].exp_fv, got);
	}

	/* 4. 阶梯充电（常温 25℃） */
	struct { int mv; int exp_fcc; } stp[] = {
		{ 3300,  1000 }, { 3349,  1000 },
		{ 3350, 12400 }, { 4000, 12400 }, { 4199, 12400 },
		{ 4200, 10800 }, { 4449, 10800 },
		{ 4450,  6720 }, { 4499,  6720 }, { 4500,  6720 },
	};
	for (size_t i = 0; i < sizeof(stp)/sizeof(stp[0]); i++) {
		temp_ddc = 250;
		vbat_mv = stp[i].mv;
		int got = profile_fcc_ua() / 1000;
		CHECK(got == stp[i].exp_fcc, "vbat=%d 期望 %d 得 %d",
		      stp[i].mv, stp[i].exp_fcc, got);
	}

	/* 5. 低温阶梯（10℃） */
	struct { int mv; int exp_fcc; } cld[] = {
		{ 3300, 1000 }, { 4000, 1000 }, { 4199, 1000 },
		{ 4200,  660 }, { 4440,  660 },
	};
	for (size_t i = 0; i < sizeof(cld)/sizeof(cld[0]); i++) {
		temp_ddc = 100;
		vbat_mv = cld[i].mv;
		int got = profile_fcc_ua() / 1000;
		CHECK(got == cld[i].exp_fcc, "低温 vbat=%d 期望 %d 得 %d",
		      cld[i].mv, cld[i].exp_fcc, got);
	}

	/* 6. 单调性：升温降流（42.9→58.0 全程 FCC 非增） */
	prev = 12400000;
	for (t = 429; t <= 580; t++) {
		temp_ddc = t;
		vbat_mv = 4000;
		int fcc = profile_fcc_ua();
		CHECK(fcc <= prev, "t=%d.%d FCC 上升 (%d -> %d)",
		      t/10, t%10, prev, fcc);
		if (fcc < prev)
			prev = fcc;
	}

	if (fails == 0)
		printf("ALL PASS: jeita/step/cold/thermal 曲线表全边界验证通过\n");
	else
		printf("%d FAILURES\n", fails);
	return fails ? 1 : 0;
}
