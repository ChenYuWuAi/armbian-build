// SPDX-License-Identifier: GPL-2.0-only
/*
 * elish_batt_agg.c - Mi Pad 5 Pro (elish) 2S 电池包聚合器
 *
 * 背景：elish 用两颗独立的 bq27z561 电量计（bq27z561-0 / bq27z561-1）串成 2S
 * 电池包。mainline/Armbian 下没有原厂 dual_fuel_gauge_class 那样的聚合节点，
 * 所以 btop / upower / GNOME 只会挑到其中一颗电芯，看到的功率大约只有整包的一半
 * （例：单芯 4.15V x 4.0A = 16.6W，而整包实际是 34W）。
 *
 * 本模块把两颗表计聚合成一个 "BAT0" power_supply（用标准名，Vitals/upower 才认得）：
 *   voltage_now = V0 + V1            整包电压
 *   current_now = (I0 + I1) / 2      串联电流相同，取平均
 *   power_now   = V0*I0 + V1*I1      整包功率（btop 直接取这个值算瓦数）
 *   capacity    = min(C0, C1)        取更保守的一颗
 *   temp        = max(T0, T1)
 *   status      = Charging > Discharging > Full > Not charging
 *
 * 不依赖设备树改动（不需要重刷 DTB），只按名字找已有的表计节点。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define ELISH_NUM_CELLS		2
#define ELISH_POLL_MS		2000

static const char *const elish_cell_name[ELISH_NUM_CELLS] = {
	"bq27z561-0",
	"bq27z561-1",
};

struct elish_batt {
	struct power_supply *cells[ELISH_NUM_CELLS];
	struct power_supply *psy;
	struct delayed_work poll;
	struct mutex lock;		/* 保护下面这些缓存值 */
	int status;
	int capacity;
	int vnow;			/* uV，整包 */
	int inow;			/* uA，串联电流 */
	int pnow;			/* uW，整包功率 */
	int temp;			/* 0.1C */
	int present;
};

static struct elish_batt *eb;

static int cell_get(struct power_supply *psy, enum power_supply_property p,
		    int *out)
{
	union power_supply_propval v;
	int ret;

	ret = power_supply_get_property(psy, p, &v);
	if (ret)
		return ret;
	*out = v.intval;
	return 0;
}

static void elish_batt_poll(struct work_struct *w)
{
	struct elish_batt *b = container_of(to_delayed_work(w),
					    struct elish_batt, poll);
	int i, n = 0;
	int vmax = 0, isum = 0, tmax = -1000, tmin = 1000;
	s64 cap_num = 0;
	int cap_den = 0;
	int any_chg = 0, any_dis = 0, any_full = 0, any_present = 0;
	int status;

	for (i = 0; i < ELISH_NUM_CELLS; i++) {
		struct power_supply *psy = b->cells[i];
		int v = 0, c = 0, t = 0, s = 0, p = 0;

		if (!psy) {
			/* 表计可能比本模块晚注册，补一次查找 */
			psy = power_supply_get_by_name(elish_cell_name[i]);
			if (psy)
				b->cells[i] = psy;
			else
				continue;
		}

		if (cell_get(psy, POWER_SUPPLY_PROP_PRESENT, &p) == 0 &&
		    p == 0)
			continue;

		n++;
		any_present = 1;

		if (cell_get(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &v) == 0 &&
		    v > vmax)
			vmax = v;
		if (cell_get(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &c) == 0)
			isum += c;
		if (cell_get(psy, POWER_SUPPLY_PROP_TEMP, &t) == 0) {
			if (t > tmax)
				tmax = t;
			if (t < tmin)
				tmin = t;
		}
		if (cell_get(psy, POWER_SUPPLY_PROP_CAPACITY, &c) == 0 &&
		    cell_get(psy, POWER_SUPPLY_PROP_CHARGE_FULL, &v) == 0 &&
		    v > 0) {
			cap_num += (s64)v * c;   /* 按 FCC 加权（原厂 fg_read_system_soc） */
			cap_den += v;
		}
		if (cell_get(psy, POWER_SUPPLY_PROP_STATUS, &s) == 0) {
			if (s == POWER_SUPPLY_STATUS_CHARGING)
				any_chg = 1;
			else if (s == POWER_SUPPLY_STATUS_DISCHARGING)
				any_dis = 1;
			else if (s == POWER_SUPPLY_STATUS_FULL)
				any_full = 1;
		}
	}

	if (any_chg)
		status = POWER_SUPPLY_STATUS_CHARGING;
	else if (any_dis)
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (any_full)
		status = POWER_SUPPLY_STATUS_FULL;
	else if (any_present)
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else
		status = POWER_SUPPLY_STATUS_UNKNOWN;

	/* 合成规则照抄原厂 dual_fuel_gauge_class.c（本机电池是 1S2P：
	 * DT battery-pack = 4.45V / 8600mAh / 33.2Wh，原厂 fv-max-uv = 4.5V）：
	 *   电压 fg_read_volt()        = MAX(两芯)
	 *   电流 fg_read_current()     = 两芯之和
	 *   容量 fg_read_system_soc()  = 按两颗 FCC 加权平均
	 *   温度 fg_read_temperature() = 任一颗 <=15.0C 取 MIN，否则取 MAX
	 *   功率 = 电压 * 电流 */
	int new_status = status;
	int new_cap = cap_den > 0 ? (int)((cap_num + cap_den / 2) / cap_den) : 0;
	int new_v = n ? vmax : 0;
	int new_i = n ? isum : 0;
	int new_p = n ? (int)div_s64((s64)vmax * isum, 1000000) : 0;
	int new_t = n ? ((tmin <= 150) ? tmin : tmax) : 0;
	bool changed;

	mutex_lock(&b->lock);
	changed = (b->status != new_status || b->capacity != new_cap ||
		   b->vnow != new_v || b->inow != new_i ||
		   b->pnow != new_p || b->temp != new_t ||
		   b->present != any_present);
	b->status = new_status;
	b->capacity = new_cap;
	b->vnow = new_v;
	b->inow = new_i;
	b->pnow = new_p;
	b->temp = new_t;
	b->present = any_present;
	mutex_unlock(&b->lock);

	/* 值有变化才发 uevent，避免每 2s 无谓唤醒 upower */
	if (changed && b->psy)
		power_supply_changed(b->psy);

	schedule_delayed_work(&b->poll, msecs_to_jiffies(ELISH_POLL_MS));
}

static enum power_supply_property elish_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int elish_batt_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct elish_batt *b = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&b->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = b->status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = b->present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = b->capacity;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = b->vnow;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = b->inow;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = b->pnow;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = b->temp;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&b->lock);

	return ret;
}

static const struct power_supply_desc elish_batt_desc = {
	/* 用标准名字 BAT0：GNOME/Vitals 这类工具只认硬编码的 BAT0..BAT2/BATT/CMB* */
	.name		= "BAT0",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= elish_batt_props,
	.num_properties	= ARRAY_SIZE(elish_batt_props),
	.get_property	= elish_batt_get_property,
};

static int __init elish_batt_init(void)
{
	struct power_supply_config cfg = {};
	int i, ret;

	eb = kzalloc(sizeof(*eb), GFP_KERNEL);
	if (!eb)
		return -ENOMEM;

	mutex_init(&eb->lock);
	INIT_DELAYED_WORK(&eb->poll, elish_batt_poll);

	for (i = 0; i < ELISH_NUM_CELLS; i++) {
		eb->cells[i] = power_supply_get_by_name(elish_cell_name[i]);
		if (eb->cells[i])
			dev_info(eb->cells[i]->dev.parent,
				 "elish-batt-agg: found %s\n",
				 elish_cell_name[i]);
	}

	if (!eb->cells[0] && !eb->cells[1]) {
		pr_err("elish-batt-agg: no fuel gauge found\n");
		ret = -ENODEV;
		goto err_free;
	}

	cfg.drv_data = eb;
	/* 设计容量指向整包节点 battery-pack（4.45V / 8600mAh / 33.2Wh）。默认会从
	 * 表计节点继承单芯的 4.3Ah / 16.6Wh，upower/Vitals 会看到一半容量。 */
	cfg.of_node = of_find_node_by_name(NULL, "battery-pack");
	eb->psy = power_supply_register(eb->cells[0] ? &eb->cells[0]->dev : NULL,
					&elish_batt_desc, &cfg);
	if (IS_ERR(eb->psy)) {
		ret = PTR_ERR(eb->psy);
		pr_err("elish-batt-agg: register failed: %d\n", ret);
		goto err_put;
	}

	schedule_delayed_work(&eb->poll, 0);
	pr_info("elish-batt-agg: battery pack aggregator ready\n");
	return 0;

err_put:
	for (i = 0; i < ELISH_NUM_CELLS; i++)
		if (eb->cells[i])
			power_supply_put(eb->cells[i]);
err_free:
	kfree(eb);
	eb = NULL;
	return ret;
}

static void __exit elish_batt_exit(void)
{
	int i;

	if (!eb)
		return;

	cancel_delayed_work_sync(&eb->poll);
	if (eb->psy)
		power_supply_unregister(eb->psy);
	for (i = 0; i < ELISH_NUM_CELLS; i++)
		if (eb->cells[i])
			power_supply_put(eb->cells[i]);
	kfree(eb);
	eb = NULL;
}

module_init(elish_batt_init);
module_exit(elish_batt_exit);

MODULE_AUTHOR("axis");
MODULE_DESCRIPTION("Xiaomi Mi Pad 5 Pro (elish) 2S battery pack aggregator");
MODULE_LICENSE("GPL");
