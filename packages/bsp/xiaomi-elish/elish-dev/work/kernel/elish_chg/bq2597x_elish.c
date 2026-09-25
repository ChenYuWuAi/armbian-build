/*
 * BQ2570x battery charging driver
 *
 * Copyright (C) 2017 Texas Instruments *
 * Copyright (C) 2021 XiaoMi, Inc.
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#define pr_fmt(fmt)	"[bq2597x] %s: " fmt, __func__

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include <asm/neon.h>
#include "bq25970_reg.h"
/*#include "bq2597x.h"*/

enum {
	VBUS_ERROR_NONE,
	VBUS_ERROR_LOW,
	VBUS_ERROR_HIGH,
};

enum {
	ADC_IBUS,
	ADC_VBUS,
	ADC_VAC,
	ADC_VOUT,
	ADC_VBAT,
	ADC_IBAT,
	ADC_TBUS,
	ADC_TBAT,
	ADC_TDIE,
	ADC_MAX_NUM,
};

/* SC8551 support dropped: elish uses BQ25970 */

#define BQ25970_ROLE_STDALONE   0
#define BQ25970_ROLE_SLAVE	1
#define BQ25970_ROLE_MASTER	2

enum {
	BQ25970_STDALONE,
	BQ25970_SLAVE,
	BQ25970_MASTER,
};

enum {
	BQ25968,
	BQ25970,
	SC8551,
};

static int bq2597x_mode_data[] = {
	[BQ25970_STDALONE] = BQ25970_STDALONE,
	[BQ25970_MASTER] = BQ25970_ROLE_MASTER,
	[BQ25970_SLAVE] = BQ25970_ROLE_SLAVE,
};


#define	BAT_OVP_ALARM		BIT(7)
#define BAT_OCP_ALARM		BIT(6)
#define	BUS_OVP_ALARM		BIT(5)
#define	BUS_OCP_ALARM		BIT(4)
#define	BAT_UCP_ALARM		BIT(3)
#define	VBUS_INSERT		BIT(2)
#define VBAT_INSERT		BIT(1)
#define	ADC_DONE		BIT(0)

#define BAT_OVP_FAULT		BIT(7)
#define BAT_OCP_FAULT		BIT(6)
#define BUS_OVP_FAULT		BIT(5)
#define BUS_OCP_FAULT		BIT(4)
#define TBUS_TBAT_ALARM		BIT(3)
#define TS_BAT_FAULT		BIT(2)
#define	TS_BUS_FAULT		BIT(1)
#define	TS_DIE_FAULT		BIT(0)

/*below used for comm with other module*/
#define	BAT_OVP_FAULT_SHIFT			0
#define	BAT_OCP_FAULT_SHIFT			1
#define	BUS_OVP_FAULT_SHIFT			2
#define	BUS_OCP_FAULT_SHIFT			3
#define	BAT_THERM_FAULT_SHIFT			4
#define	BUS_THERM_FAULT_SHIFT			5
#define	DIE_THERM_FAULT_SHIFT			6

#define	BAT_OVP_FAULT_MASK		(1 << BAT_OVP_FAULT_SHIFT)
#define	BAT_OCP_FAULT_MASK		(1 << BAT_OCP_FAULT_SHIFT)
#define	BUS_OVP_FAULT_MASK		(1 << BUS_OVP_FAULT_SHIFT)
#define	BUS_OCP_FAULT_MASK		(1 << BUS_OCP_FAULT_SHIFT)
#define	BAT_THERM_FAULT_MASK		(1 << BAT_THERM_FAULT_SHIFT)
#define	BUS_THERM_FAULT_MASK		(1 << BUS_THERM_FAULT_SHIFT)
#define	DIE_THERM_FAULT_MASK		(1 << DIE_THERM_FAULT_SHIFT)

#define	BAT_OVP_ALARM_SHIFT			0
#define	BAT_OCP_ALARM_SHIFT			1
#define	BUS_OVP_ALARM_SHIFT			2
#define	BUS_OCP_ALARM_SHIFT			3
#define	BAT_THERM_ALARM_SHIFT			4
#define	BUS_THERM_ALARM_SHIFT			5
#define	DIE_THERM_ALARM_SHIFT			6
#define BAT_UCP_ALARM_SHIFT			7

#define	BAT_OVP_ALARM_MASK		(1 << BAT_OVP_ALARM_SHIFT)
#define	BAT_OCP_ALARM_MASK		(1 << BAT_OCP_ALARM_SHIFT)
#define	BUS_OVP_ALARM_MASK		(1 << BUS_OVP_ALARM_SHIFT)
#define	BUS_OCP_ALARM_MASK		(1 << BUS_OCP_ALARM_SHIFT)
#define	BAT_THERM_ALARM_MASK		(1 << BAT_THERM_ALARM_SHIFT)
#define	BUS_THERM_ALARM_MASK		(1 << BUS_THERM_ALARM_SHIFT)
#define	DIE_THERM_ALARM_MASK		(1 << DIE_THERM_ALARM_SHIFT)
#define	BAT_UCP_ALARM_MASK		(1 << BAT_UCP_ALARM_SHIFT)

#define VBAT_REG_STATUS_SHIFT			0
#define IBAT_REG_STATUS_SHIFT			1

#define VBAT_REG_STATUS_MASK		(1 << VBAT_REG_STATUS_SHIFT)
#define IBAT_REG_STATUS_MASK		(1 << VBAT_REG_STATUS_SHIFT)

#define bq_err(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_ERR "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_ERR "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_ERR "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

#define bq_info(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_INFO "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_INFO "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_INFO "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

#define bq_dbg(fmt, ...)								\
do {											\
	if (bq->mode == BQ25970_ROLE_MASTER)						\
		printk(KERN_DEBUG "[bq2597x-MASTER]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else if (bq->mode == BQ25970_ROLE_SLAVE)					\
		printk(KERN_DEBUG "[bq2597x-SLAVE]:%s:" fmt, __func__, ##__VA_ARGS__);	\
	else										\
		printk(KERN_DEBUG "[bq2597x-STANDALONE]:%s:" fmt, __func__, ##__VA_ARGS__);\
} while (0);

enum hvdcp3_type {
	HVDCP3_NONE = 0,
	HVDCP3_CLASSA_18W,
	HVDCP3_CLASSB_27W,
	HVDCP3P5_CLASSA_18W,
	HVDCP3P5_CLASSB_27W,
};

#define BUS_OVP_FOR_QC			10500
#define BUS_OVP_ALARM_FOR_QC			9500
#define BUS_OCP_FOR_QC_CLASS_A			3250
#define BUS_OCP_ALARM_FOR_QC_CLASS_A			2000
#define BUS_OCP_FOR_QC_CLASS_B			4000
#define BUS_OCP_ALARM_FOR_QC_CLASS_B			3000
#define BUS_OCP_FOR_QC3P5_CLASS_A			3000
#define BUS_OCP_ALARM_FOR_QC3P5_CLASS_A		2500
#define BUS_OCP_FOR_QC3P5_CLASS_B			3500
#define BUS_OCP_ALARM_FOR_QC3P5_CLASS_B		3200

/*end*/

struct bq2597x_cfg {
	bool bat_ovp_disable;
	bool bat_ocp_disable;
	bool bat_ovp_alm_disable;
	bool bat_ocp_alm_disable;

	int bat_ovp_th;
	int bat_ovp_alm_th;
	int bat_ocp_th;
	int bat_ocp_alm_th;

	bool bus_ovp_alm_disable;
	bool bus_ocp_disable;
	bool bus_ocp_alm_disable;

	int bus_ovp_th;
	int bus_ovp_alm_th;
	int bus_ocp_th;
	int bus_ocp_alm_th;

	bool bat_ucp_alm_disable;

	int bat_ucp_alm_th;
	int ac_ovp_th;

	bool bat_therm_disable;
	bool bus_therm_disable;
	bool die_therm_disable;

	int bat_therm_th; /*in %*/
	int bus_therm_th; /*in %*/
	int die_therm_th; /*in degC*/

	int sense_r_mohm;
};

struct bq2597x {
	struct device *dev;
	struct i2c_client *client;

	int part_no;
	int revision;

	int chip_vendor;
	int mode;

	struct mutex data_lock;
	struct mutex i2c_rw_lock;
	struct mutex charging_disable_lock;
	struct mutex irq_complete;

	bool irq_waiting;
	bool irq_disabled;
	bool resume_completed;

	bool batt_present;
	bool vbus_present;

	bool usb_present;
	bool charge_enabled;	/* Register bit status */

	/* ADC reading */
	int vbat_volt;
	int vbus_volt;
	int vout_volt;
	int vac_volt;

	int ibat_curr;
	int ibus_curr;

	int bat_temp;
	int bus_temp;
	int die_temp;

	/* alarm/fault status */
	bool bat_ovp_fault;
	bool bat_ocp_fault;
	bool bus_ovp_fault;
	bool bus_ocp_fault;

	bool bat_ovp_alarm;
	bool bat_ocp_alarm;
	bool bus_ovp_alarm;
	bool bus_ocp_alarm;

	bool bat_ucp_alarm;

	bool bat_therm_alarm;
	bool bus_therm_alarm;
	bool die_therm_alarm;

	bool bat_therm_fault;
	bool bus_therm_fault;
	bool die_therm_fault;

	bool therm_shutdown_flag;
	bool therm_shutdown_stat;

	bool vbat_reg;
	bool ibat_reg;

	int  prev_alarm;
	int  prev_fault;

	int chg_ma;
	int chg_mv;

	int charge_state;

	struct bq2597x_cfg *cfg;

	int skip_writes;
	int skip_reads;

	struct bq2597x_platform_data *platform_data;

	struct delayed_work monitor_work;

	struct dentry *debug_root;

	struct power_supply_desc psy_desc;
	struct power_supply_config psy_cfg;
	struct power_supply *fc2_psy;
};

static int bq2597x_set_acovp_th(struct bq2597x *bq, int threshold);
static int bq2597x_set_busovp_th(struct bq2597x *bq, int threshold);

/************************************************************************/
static int __bq2597x_read_byte(struct bq2597x *bq, u8 reg, u8 *data)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(bq->client, reg);
	if (ret < 0) {
		bq_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u8) ret;

	return 0;
}

static int __bq2597x_write_byte(struct bq2597x *bq, int reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(bq->client, reg, val);
	if (ret < 0) {
		bq_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
		       val, reg, ret);
		return ret;
	}
	return 0;
}

static int __bq2597x_read_word(struct bq2597x *bq, u8 reg, u16 *data)
{
	s32 ret;

	ret = i2c_smbus_read_word_data(bq->client, reg);
	if (ret < 0) {
		bq_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u16) ret;

	return 0;
}

static int bq2597x_read_byte(struct bq2597x *bq, u8 reg, u8 *data)
{
	int ret;

	if (bq->skip_reads) {
		*data = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_byte(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_write_byte(struct bq2597x *bq, u8 reg, u8 data)
{
	int ret;

	if (bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_write_byte(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_read_word(struct bq2597x *bq, u8 reg, u16 *data)
{
	int ret;

	if (bq->skip_reads) {
		*data = 0;
		return 0;
	}

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_word(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int bq2597x_update_bits(struct bq2597x *bq, u8 reg,
				    u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	if (bq->skip_reads || bq->skip_writes)
		return 0;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __bq2597x_read_byte(bq, reg, &tmp);
	if (ret) {
		bq_err("Failed: reg=%02X, ret=%d\n", reg, ret);
		goto out;
	}

	tmp &= ~mask;
	tmp |= data & mask;

	ret = __bq2597x_write_byte(bq, reg, tmp);
	if (ret)
		bq_err("Failed: reg=%02X, ret=%d\n", reg, ret);

out:
	mutex_unlock(&bq->i2c_rw_lock);
	return ret;
}

static int bq2597x_enable_charge(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_CHG_ENABLE;
	else
		val = BQ2597X_CHG_DISABLE;

	val <<= BQ2597X_CHG_EN_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_CHG_EN_MASK, val);

	return ret;
}

static int bq2597x_check_charge_enabled(struct bq2597x *bq, bool *enabled)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0C, &val);
	if (!ret)
		*enabled = !!(val & BQ2597X_CHG_EN_MASK);
	return ret;
}

static int bq2597x_enable_wdt(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_WATCHDOG_ENABLE;
	else
		val = BQ2597X_WATCHDOG_DISABLE;

	val <<= BQ2597X_WATCHDOG_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0B,
				BQ2597X_WATCHDOG_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_wdt(struct bq2597x *bq, int ms)
{
	int ret;
	u8 val;

	if (ms == 500)
		val = BQ2597X_WATCHDOG_0P5S;
	else if (ms == 1000)
		val = BQ2597X_WATCHDOG_1S;
	else if (ms == 5000)
		val = BQ2597X_WATCHDOG_5S;
	else if (ms == 30000)
		val = BQ2597X_WATCHDOG_30S;
	else
		val = BQ2597X_WATCHDOG_30S;

	val <<= BQ2597X_WATCHDOG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0B,
				BQ2597X_WATCHDOG_MASK, val);
	return ret;
}

static int bq2597x_enable_batovp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OVP_ENABLE;
	else
		val = BQ2597X_BAT_OVP_DISABLE;

	val <<= BQ2597X_BAT_OVP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_00,
				BQ2597X_BAT_OVP_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_batovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OVP_BASE)
		threshold = BQ2597X_BAT_OVP_BASE;

	val = (threshold - BQ2597X_BAT_OVP_BASE) / BQ2597X_BAT_OVP_LSB;

	val <<= BQ2597X_BAT_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_00,
				BQ2597X_BAT_OVP_MASK, val);
	return ret;
}

static int bq2597x_enable_batovp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OVP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_OVP_ALM_DISABLE;

	val <<= BQ2597X_BAT_OVP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_01,
				BQ2597X_BAT_OVP_ALM_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_batovp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OVP_ALM_BASE)
		threshold = BQ2597X_BAT_OVP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_OVP_ALM_BASE) / BQ2597X_BAT_OVP_ALM_LSB;

	val <<= BQ2597X_BAT_OVP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_01,
				BQ2597X_BAT_OVP_ALM_MASK, val);
	return ret;
}

static int bq2597x_enable_batocp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OCP_ENABLE;
	else
		val = BQ2597X_BAT_OCP_DISABLE;

	val <<= BQ2597X_BAT_OCP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_02,
				BQ2597X_BAT_OCP_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_batocp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OCP_BASE)
		threshold = BQ2597X_BAT_OCP_BASE;

	val = (threshold - BQ2597X_BAT_OCP_BASE) / BQ2597X_BAT_OCP_LSB;

	val <<= BQ2597X_BAT_OCP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_02,
				BQ2597X_BAT_OCP_MASK, val);
	return ret;
}

static int bq2597x_enable_batocp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_OCP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_OCP_ALM_DISABLE;

	val <<= BQ2597X_BAT_OCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_03,
				BQ2597X_BAT_OCP_ALM_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_batocp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_OCP_ALM_BASE)
		threshold = BQ2597X_BAT_OCP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_OCP_ALM_BASE) / BQ2597X_BAT_OCP_ALM_LSB;

	val <<= BQ2597X_BAT_OCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_03,
				BQ2597X_BAT_OCP_ALM_MASK, val);
	return ret;
}


static int bq2597x_set_busovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OVP_BASE)
		threshold = BQ2597X_BUS_OVP_BASE;

	val = (threshold - BQ2597X_BUS_OVP_BASE) / BQ2597X_BUS_OVP_LSB;

	val <<= BQ2597X_BUS_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_06,
				BQ2597X_BUS_OVP_MASK, val);
	return ret;
}

static int bq2597x_enable_busovp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OVP_ALM_ENABLE;
	else
		val = BQ2597X_BUS_OVP_ALM_DISABLE;

	val <<= BQ2597X_BUS_OVP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_07,
				BQ2597X_BUS_OVP_ALM_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_busovp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OVP_ALM_BASE)
		threshold = BQ2597X_BUS_OVP_ALM_BASE;

	val = (threshold - BQ2597X_BUS_OVP_ALM_BASE) / BQ2597X_BUS_OVP_ALM_LSB;

	val <<= BQ2597X_BUS_OVP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_07,
				BQ2597X_BUS_OVP_ALM_MASK, val);
	return ret;
}

static int bq2597x_enable_busocp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OCP_ENABLE;
	else
		val = BQ2597X_BUS_OCP_DISABLE;

	val <<= BQ2597X_BUS_OCP_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_08,
				BQ2597X_BUS_OCP_DIS_MASK, val);
	return ret;
}


static int bq2597x_set_busocp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OCP_BASE)
		threshold = BQ2597X_BUS_OCP_BASE;

	val = (threshold - BQ2597X_BUS_OCP_BASE) / BQ2597X_BUS_OCP_LSB;

	val <<= BQ2597X_BUS_OCP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_08,
				BQ2597X_BUS_OCP_MASK, val);
	return ret;
}

static int bq2597x_enable_busocp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BUS_OCP_ALM_ENABLE;
	else
		val = BQ2597X_BUS_OCP_ALM_DISABLE;

	val <<= BQ2597X_BUS_OCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_09,
				BQ2597X_BUS_OCP_ALM_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_busocp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BUS_OCP_ALM_BASE)
		threshold = BQ2597X_BUS_OCP_ALM_BASE;

	val = (threshold - BQ2597X_BUS_OCP_ALM_BASE) / BQ2597X_BUS_OCP_ALM_LSB;

	val <<= BQ2597X_BUS_OCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_09,
				BQ2597X_BUS_OCP_ALM_MASK, val);
	return ret;
}

static int bq2597x_enable_batucp_alarm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_BAT_UCP_ALM_ENABLE;
	else
		val = BQ2597X_BAT_UCP_ALM_DISABLE;

	val <<= BQ2597X_BAT_UCP_ALM_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_04,
				BQ2597X_BAT_UCP_ALM_DIS_MASK, val);
	return ret;
}

static int bq2597x_set_batucp_alarm_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_BAT_UCP_ALM_BASE)
		threshold = BQ2597X_BAT_UCP_ALM_BASE;

	val = (threshold - BQ2597X_BAT_UCP_ALM_BASE) / BQ2597X_BAT_UCP_ALM_LSB;

	val <<= BQ2597X_BAT_UCP_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_04,
				BQ2597X_BAT_UCP_ALM_MASK, val);
	return ret;
}

static int bq2597x_set_acovp_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold < BQ2597X_AC_OVP_BASE)
		threshold = BQ2597X_AC_OVP_BASE;

	if (threshold == BQ2597X_AC_OVP_6P5V)
		val = 0x07;
	else
		val = (threshold - BQ2597X_AC_OVP_BASE) /  BQ2597X_AC_OVP_LSB;

	val <<= BQ2597X_AC_OVP_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_AC_OVP_MASK, val);

	return ret;

}

static int bq2597x_set_vdrop_th(struct bq2597x *bq, int threshold)
{
	int ret;
	u8 val;

	if (threshold == 300)
		val = BQ2597X_VDROP_THRESHOLD_300MV;
	else
		val = BQ2597X_VDROP_THRESHOLD_400MV;

	val <<= BQ2597X_VDROP_THRESHOLD_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_VDROP_THRESHOLD_SET_MASK,
				val);

	return ret;
}

static int bq2597x_set_vdrop_deglitch(struct bq2597x *bq, int us)
{
	int ret;
	u8 val;

	if (us == 8)
		val = BQ2597X_VDROP_DEGLITCH_8US;
	else
		val = BQ2597X_VDROP_DEGLITCH_5MS;

	val <<= BQ2597X_VDROP_DEGLITCH_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_05,
				BQ2597X_VDROP_DEGLITCH_SET_MASK,
				val);
	return ret;
}

static int bq2597x_enable_bat_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TSBAT_ENABLE;
	else
		val = BQ2597X_TSBAT_DISABLE;

	val <<= BQ2597X_TSBAT_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TSBAT_DIS_MASK, val);
	return ret;
}

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int bq2597x_set_bat_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_29, threshold);
	return ret;
}

static int bq2597x_enable_bus_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TSBUS_ENABLE;
	else
		val = BQ2597X_TSBUS_DISABLE;

	val <<= BQ2597X_TSBUS_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TSBUS_DIS_MASK, val);
	return ret;
}

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int bq2597x_set_bus_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_28, threshold);
	return ret;
}


static int bq2597x_enable_die_therm(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_TDIE_ENABLE;
	else
		val = BQ2597X_TDIE_DISABLE;

	val <<= BQ2597X_TDIE_DIS_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_0C,
				BQ2597X_TDIE_DIS_MASK, val);
	return ret;
}

/*
 * please be noted that the unit here is degC
 */
static int bq2597x_set_die_therm_th(struct bq2597x *bq, u8 threshold)
{
	int ret;
	u8 val;

	/*BE careful, LSB is here is 1/LSB, so we use multiply here*/
	val = (threshold - BQ2597X_TDIE_ALM_BASE) * BQ2597X_TDIE_ALM_LSB;
	val <<= BQ2597X_TDIE_ALM_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2A,
				BQ2597X_TDIE_ALM_MASK, val);
	return ret;
}

static int bq2597x_enable_adc(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_ADC_ENABLE;
	else
		val = BQ2597X_ADC_DISABLE;

	val <<= BQ2597X_ADC_EN_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_EN_MASK, val);
	return ret;
}

static int bq2597x_set_adc_average(struct bq2597x *bq, bool avg)
{
	int ret;
	u8 val;

	if (avg)
		val = BQ2597X_ADC_AVG_ENABLE;
	else
		val = BQ2597X_ADC_AVG_DISABLE;

	val <<= BQ2597X_ADC_AVG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_AVG_MASK, val);
	return 0;
}

static int bq2597x_set_adc_scanrate(struct bq2597x *bq, bool oneshot)
{
	int ret;
	u8 val;

	if (oneshot)
		val = BQ2597X_ADC_RATE_ONESHOT;
	else
		val = BQ2597X_ADC_RATE_CONTINOUS;

	val <<= BQ2597X_ADC_RATE_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_EN_MASK, val);
	return ret;
}

static int bq2597x_set_adc_bits(struct bq2597x *bq, int bits)
{
	int ret;
	u8 val;

	if (bits > 15)
		bits = 15;
	if (bits < 12)
		bits = 12;
	val = 15 - bits;

	val <<= BQ2597X_ADC_SAMPLE_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_14,
				BQ2597X_ADC_SAMPLE_MASK, val);
	return ret;
}

#define ADC_REG_BASE 0x16
static int bq2597x_get_adc_data(struct bq2597x *bq, int channel,  int *result)
{
	int ret;
	u16 val;
	s16 t;

	if (channel < 0 || channel >= ADC_MAX_NUM)
		return -EINVAL;

	ret = bq2597x_read_word(bq, ADC_REG_BASE + (channel << 1), &val);
	if (ret < 0)
		return ret;
	t = val & 0xFF;
	t <<= 8;
	t |= (val >> 8) & 0xFF;
	*result = t;

	return 0;
}

/* SC8551/SC8551A (Southchip bq25970 clone, id 0x51): linear raw*LSB
 * scaling, no base offset.  Integer fixed-point, float-free. */
static int bq2597x_adc_scaled(struct bq2597x *bq, int channel)
{
	int raw, base = 0, mult = 1, div = 1;
	int ret = bq2597x_get_adc_data(bq, channel, &raw);

	if (ret)
		return ret;

	if (bq->chip_vendor == SC8551) {
		switch (channel) {
		case ADC_IBUS: mult = 25;  div = 16;  break; /* 1.5625 */
		case ADC_VBUS: mult = 15;  div = 4;   break; /* 3.75   */
		case ADC_VAC:  mult = 5;   div = 1;   break; /* 5      */
		case ADC_VOUT: mult = 5;   div = 4;   break; /* 1.25   */
		case ADC_VBAT: mult = 503; div = 400; break; /* 1.2575 */
		case ADC_IBAT: mult = 25;  div = 8;   break; /* 3.125  */
		case ADC_TDIE: mult = 1;   div = 2;   break; /* 0.5    */
		default: return raw;
		}
		return raw * mult / div;
	}

	/* BQ25970: (raw - base) * lsb */
	switch (channel) {
	case ADC_IBUS: base = BQ2597X_IBUS_ADC0_BASE; mult = BQ2597X_IBUS_ADC0_LSB; break;
	case ADC_VBUS: base = BQ2597X_VBUS_ADC0_BASE; mult = BQ2597X_VBUS_ADC0_LSB; break;
	case ADC_VAC:  base = BQ2597X_VAC_ADC0_BASE;  mult = BQ2597X_VAC_ADC0_LSB;  break;
	case ADC_VBAT: base = BQ2597X_VBAT_ADC0_BASE; mult = BQ2597X_VBAT_ADC0_LSB; break;
	default: return raw;
	}
	return (raw - base) * mult;
}

static int bq2597x_set_adc_scan(struct bq2597x *bq, int channel, bool enable)
{
	int ret;
	u8 reg;
	u8 mask;
	u8 shift;
	u8 val;

	if (channel > ADC_MAX_NUM)
		return -EINVAL;

	if (channel == ADC_IBUS) {
		reg = BQ2597X_REG_14;
		shift = BQ2597X_IBUS_ADC_DIS_SHIFT;
		mask = BQ2597X_IBUS_ADC_DIS_MASK;
	} else {
		reg = BQ2597X_REG_15;
		shift = 8 - channel;
		mask = 1 << shift;
	}

	if (enable)
		val = 0 << shift;
	else
		val = 1 << shift;

	ret = bq2597x_update_bits(bq, reg, mask, val);

	return ret;
}

static int bq2597x_set_alarm_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0F, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_0F, val);

	return ret;
}

static int bq2597x_clear_alarm_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0F, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_0F, val);

	return ret;
}

static int bq2597x_set_fault_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_12, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_12, val);

	return ret;
}

static int bq2597x_clear_fault_int_mask(struct bq2597x *bq, u8 mask)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_12, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = bq2597x_write_byte(bq, BQ2597X_REG_12, val);

	return ret;
}


static int bq2597x_set_sense_resistor(struct bq2597x *bq, int r_mohm)
{
	int ret;
	u8 val;

	if (r_mohm == 2)
		val = BQ2597X_SET_IBAT_SNS_RES_2MHM;
	else if (r_mohm == 5)
		val = BQ2597X_SET_IBAT_SNS_RES_5MHM;
	else
		return -EINVAL;

	val <<= BQ2597X_SET_IBAT_SNS_RES_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_SET_IBAT_SNS_RES_MASK,
				val);
	return ret;
}

static int bq2597x_set_ibus_ucp_thr(struct bq2597x *bq, int ibus_ucp_thr)
{
	int ret;
	u8 val;

	if (ibus_ucp_thr == 300)
		val = BQ2597X_IBUS_UCP_RISE_300MA;
	else if (ibus_ucp_thr == 500)
		val = BQ2597X_IBUS_UCP_RISE_500MA;
	else
		return -EINVAL;

	val <<= BQ2597X_IBUS_UCP_RISE_TH_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_IBUS_UCP_RISE_TH_MASK,
				val);
	return ret;
}

static int bq2597x_enable_regulation(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_EN_REGULATION_ENABLE;
	else
		val = BQ2597X_EN_REGULATION_DISABLE;

	val <<= BQ2597X_EN_REGULATION_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_EN_REGULATION_MASK,
				val);

	return ret;

}

static int bq2597x_enable_ucp(struct bq2597x *bq, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = BQ2597X_IBUS_LOW_DG_5MS;
	else
		val = BQ2597X_IBUS_LOW_DG_10US;

	val <<= BQ2597X_IBUS_LOW_DG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2E,
				BQ2597X_IBUS_LOW_DG_MASK,
				val);

	return ret;

}

static int bq2597x_set_ss_timeout(struct bq2597x *bq, int timeout)
{
	int ret;
	u8 val;

	switch (timeout) {
	case 0:
		val = BQ2597X_SS_TIMEOUT_DISABLE;
		break;
	case 12:
		val = BQ2597X_SS_TIMEOUT_12P5MS;
		break;
	case 25:
		val = BQ2597X_SS_TIMEOUT_25MS;
		break;
	case 50:
		val = BQ2597X_SS_TIMEOUT_50MS;
		break;
	case 100:
		val = BQ2597X_SS_TIMEOUT_100MS;
		break;
	case 400:
		val = BQ2597X_SS_TIMEOUT_400MS;
		break;
	case 1500:
		val = BQ2597X_SS_TIMEOUT_1500MS;
		break;
	case 100000:
		val = BQ2597X_SS_TIMEOUT_100000MS;
		break;
	default:
		val = BQ2597X_SS_TIMEOUT_DISABLE;
		break;
	}

	val <<= BQ2597X_SS_TIMEOUT_SET_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2B,
				BQ2597X_SS_TIMEOUT_SET_MASK,
				val);

	return ret;
}

static int bq2597x_set_ibat_reg_th(struct bq2597x *bq, int th_ma)
{
	int ret;
	u8 val;

	if (th_ma == 200)
		val = BQ2597X_IBAT_REG_200MA;
	else if (th_ma == 300)
		val = BQ2597X_IBAT_REG_300MA;
	else if (th_ma == 400)
		val = BQ2597X_IBAT_REG_400MA;
	else if (th_ma == 500)
		val = BQ2597X_IBAT_REG_500MA;
	else
		val = BQ2597X_IBAT_REG_500MA;

	val <<= BQ2597X_IBAT_REG_SHIFT;
	ret = bq2597x_update_bits(bq, BQ2597X_REG_2C,
				BQ2597X_IBAT_REG_MASK,
				val);

	return ret;

}

static int bq2597x_set_vbat_reg_th(struct bq2597x *bq, int th_mv)
{
	int ret;
	u8 val;

	if (th_mv == 50)
		val = BQ2597X_VBAT_REG_50MV;
	else if (th_mv == 100)
		val = BQ2597X_VBAT_REG_100MV;
	else if (th_mv == 150)
		val = BQ2597X_VBAT_REG_150MV;
	else
		val = BQ2597X_VBAT_REG_200MV;

	val <<= BQ2597X_VBAT_REG_SHIFT;

	ret = bq2597x_update_bits(bq, BQ2597X_REG_2C,
				BQ2597X_VBAT_REG_MASK,
				val);

	return ret;
}


static int bq2597x_check_reg_status(struct bq2597x *bq)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_2C, &val);
	if (!ret) {
		bq->vbat_reg = !!(val & BQ2597X_VBAT_REG_ACTIVE_STAT_MASK);
		bq->ibat_reg = !!(val & BQ2597X_IBAT_REG_ACTIVE_STAT_MASK);
	}

	return ret;
}

static int bq2597x_get_work_mode(struct bq2597x *bq, int *mode)
{
	int ret;
	u8 val;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_0C, &val);

	if (ret) {
		bq_err("Failed to read operation mode register\n");
		return ret;
	}

	val = (val & BQ2597X_MS_MASK) >> BQ2597X_MS_SHIFT;
	if (val == BQ2597X_MS_MASTER)
		*mode = BQ25970_ROLE_MASTER;
	else if (val == BQ2597X_MS_SLAVE)
		*mode = BQ25970_ROLE_SLAVE;
	else
		*mode = BQ25970_ROLE_STDALONE;

	bq_info("work mode:%s\n", *mode == BQ25970_ROLE_STDALONE ? "Standalone" :
			(*mode == BQ25970_ROLE_SLAVE ? "Slave" : "Master"));
	return ret;
}

static int bq2597x_detect_device(struct bq2597x *bq)
{
	int ret;
	u8 data;

	ret = bq2597x_read_byte(bq, BQ2597X_REG_13, &data);
	if (ret == 0) {
		bq->part_no = (data & BQ2597X_DEV_ID_MASK);
		bq->part_no >>= BQ2597X_DEV_ID_SHIFT;

		pr_err("detect device:%d\n", data);
		if (data == SC8551_DEVICE_ID || data == SC8551A_DEVICE_ID)
			bq->chip_vendor = SC8551;
		else if (data == BQ25968_DEV_ID)
			bq->chip_vendor = BQ25968;
		else
			bq->chip_vendor = BQ25970;
	}

	return ret;
}

static int bq2597x_parse_dt(struct bq2597x *bq, struct device *dev)
{
	int ret;
	struct device_node *np = dev->of_node;

	bq->cfg = devm_kzalloc(dev, sizeof(struct bq2597x_cfg),
					GFP_KERNEL);

	if (!bq->cfg)
		return -ENOMEM;

	bq->cfg->bat_ovp_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ovp-disable");
	bq->cfg->bat_ocp_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ocp-disable");
	bq->cfg->bat_ovp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ovp-alarm-disable");
	bq->cfg->bat_ocp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ocp-alarm-disable");
	bq->cfg->bus_ocp_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ocp-disable");
	bq->cfg->bus_ovp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ovp-alarm-disable");
	bq->cfg->bus_ocp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-ocp-alarm-disable");
	bq->cfg->bat_ucp_alm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-ucp-alarm-disable");
	bq->cfg->bat_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,bat-therm-disable");
	bq->cfg->bus_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,bus-therm-disable");
	bq->cfg->die_therm_disable = of_property_read_bool(np,
			"ti,bq2597x,die-therm-disable");

	ret = of_property_read_u32(np, "ti,bq2597x,bat-ovp-threshold",
			&bq->cfg->bat_ovp_th);
	if (ret) {
		bq_err("failed to read bat-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bat-ovp-alarm-threshold",
			&bq->cfg->bat_ovp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ovp-alarm-threshold\n");
		return ret;
	}
	/*ret = of_property_read_u32(np, "ti,bq2597x,bat-ocp-threshold",
			&bq->cfg->bat_ocp_th);
	if (ret) {
		bq_err("failed to read bat-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bat-ocp-alarm-threshold",
			&bq->cfg->bat_ocp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ocp-alarm-threshold\n");
		return ret;
	}*/
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ovp-threshold",
			&bq->cfg->bus_ovp_th);
	if (ret) {
		bq_err("failed to read bus-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ovp-alarm-threshold",
			&bq->cfg->bus_ovp_alm_th);
	if (ret) {
		bq_err("failed to read bus-ovp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ocp-threshold",
			&bq->cfg->bus_ocp_th);
	if (ret) {
		bq_err("failed to read bus-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-ocp-alarm-threshold",
			&bq->cfg->bus_ocp_alm_th);
	if (ret) {
		bq_err("failed to read bus-ocp-alarm-threshold\n");
		return ret;
	}
	/*ret = of_property_read_u32(np, "ti,bq2597x,bat-ucp-alarm-threshold",
			&bq->cfg->bat_ucp_alm_th);
	if (ret) {
		bq_err("failed to read bat-ucp-alarm-threshold\n");
		return ret;
	}*/
	ret = of_property_read_u32(np, "ti,bq2597x,bat-therm-threshold",
			&bq->cfg->bat_therm_th);
	if (ret) {
		bq_err("failed to read bat-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,bus-therm-threshold",
			&bq->cfg->bus_therm_th);
	if (ret) {
		bq_err("failed to read bus-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "ti,bq2597x,die-therm-threshold",
			&bq->cfg->die_therm_th);
	if (ret) {
		bq_err("failed to read die-therm-threshold\n");
		return ret;
	}

	ret = of_property_read_u32(np, "ti,bq2597x,ac-ovp-threshold",
			&bq->cfg->ac_ovp_th);
	if (ret) {
		bq_err("failed to read ac-ovp-threshold\n");
		return ret;
	}

	if (bq->chip_vendor == SC8551) {
		/* elish: optional - fall back to the ti,bq2597x value */
		ret = of_property_read_u32(np, "sc8551,ac-ovp-threshold",
				&bq->cfg->ac_ovp_th);
		if (ret)
			bq_err("sc8551 ac-ovp missing, keep ti value\n");
	}

	/*ret = of_property_read_u32(np, "ti,bq2597x,sense-resistor-mohm",
			&bq->cfg->sense_r_mohm);
	if (ret) {
		bq_err("failed to read sense-resistor-mohm\n");
		return ret;
	}*/


	return 0;
}

static int bq2597x_init_protection(struct bq2597x *bq)
{
	int ret;

	ret = bq2597x_enable_batovp(bq, !bq->cfg->bat_ovp_disable);
	bq_info("%s bat ovp %s\n",
		bq->cfg->bat_ovp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	/* elish: force-enable bat OCP as a hardware backstop (stock disables
	 * it and never programs a threshold; each pump carries ~6A max, so
	 * 8 A trips only on real faults). */
	ret = bq2597x_enable_batocp(bq, true);
	bq_info("bat ocp forced enable %s\n",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batovp_alarm(bq, !bq->cfg->bat_ovp_alm_disable);
	bq_info("%s bat ovp alarm %s\n",
		bq->cfg->bat_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batocp_alarm(bq, !bq->cfg->bat_ocp_alm_disable);
	bq_info("%s bat ocp alarm %s\n",
		bq->cfg->bat_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_batucp_alarm(bq, !bq->cfg->bat_ucp_alm_disable);
	bq_info("%s bat ocp alarm %s\n",
		bq->cfg->bat_ucp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busovp_alarm(bq, !bq->cfg->bus_ovp_alm_disable);
	bq_info("%s bus ovp alarm %s\n",
		bq->cfg->bus_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busocp(bq, !bq->cfg->bus_ocp_disable);
	bq_info("%s bus ocp %s\n",
		bq->cfg->bus_ocp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_busocp_alarm(bq, !bq->cfg->bus_ocp_alm_disable);
	bq_info("%s bus ocp alarm %s\n",
		bq->cfg->bus_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_bat_therm(bq, !bq->cfg->bat_therm_disable);
	bq_info("%s bat therm %s\n",
		bq->cfg->bat_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_bus_therm(bq, !bq->cfg->bus_therm_disable);
	bq_info("%s bus therm %s\n",
		bq->cfg->bus_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_enable_die_therm(bq, !bq->cfg->die_therm_disable);
	bq_info("%s die therm %s\n",
		bq->cfg->die_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = bq2597x_set_batovp_th(bq, bq->cfg->bat_ovp_th);
	bq_info("set bat ovp th %d %s\n", bq->cfg->bat_ovp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batovp_alarm_th(bq, bq->cfg->bat_ovp_alm_th);
	bq_info("set bat ovp alarm threshold %d %s\n", bq->cfg->bat_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batocp_th(bq, 8000);
	bq_info("set bat ocp threshold 8000 %s\n",
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batocp_alarm_th(bq, 8000);
	bq_info("set bat ocp alarm threshold 8000 %s\n",
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
	bq_info("set bus ovp threshold %d %s\n", bq->cfg->bus_ovp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busovp_alarm_th(bq, bq->cfg->bus_ovp_alm_th);
	bq_info("set bus ovp alarm threshold %d %s\n", bq->cfg->bus_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busocp_th(bq, bq->cfg->bus_ocp_th);
	bq_info("set bus ocp threshold %d %s\n", bq->cfg->bus_ocp_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_busocp_alarm_th(bq, bq->cfg->bus_ocp_alm_th);
	bq_info("set bus ocp alarm th %d %s\n", bq->cfg->bus_ocp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_batucp_alarm_th(bq, bq->cfg->bat_ucp_alm_th);
	bq_info("set bat ucp threshold %d %s\n", bq->cfg->bat_ucp_alm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_bat_therm_th(bq, bq->cfg->bat_therm_th);
	bq_info("set die therm threshold %d %s\n", bq->cfg->bat_therm_th,
		!ret ? "successfully" : "failed");
	ret = bq2597x_set_bus_therm_th(bq, bq->cfg->bus_therm_th);
	bq_info("set bus therm threshold %d %s\n", bq->cfg->bus_therm_th,
		!ret ? "successfully" : "failed");
	ret = bq2597x_set_die_therm_th(bq, bq->cfg->die_therm_th);
	bq_info("set die therm threshold %d %s\n", bq->cfg->die_therm_th,
		!ret ? "successfully" : "failed");

	ret = bq2597x_set_acovp_th(bq, bq->cfg->ac_ovp_th);
	bq_info("set ac ovp threshold %d %s\n", bq->cfg->ac_ovp_th,
		!ret ? "successfully" : "failed");

	return 0;
}

static int bq2597x_set_bus_protection(struct bq2597x *bq, int hvdcp3_type)
{
	/* just return now, to do later */
	//return 0;

	pr_err("hvdcp3_type: %d\n", hvdcp3_type);
	if (hvdcp3_type == HVDCP3_CLASSA_18W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC_CLASS_A);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC_CLASS_A);
	} else if (hvdcp3_type == HVDCP3_CLASSB_27W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC_CLASS_B);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC_CLASS_B);
	} else if (hvdcp3_type == HVDCP3P5_CLASSA_18W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC3P5_CLASS_A);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC3P5_CLASS_A);
	} else if (hvdcp3_type == HVDCP3P5_CLASSB_27W) {
		bq2597x_set_busovp_th(bq, BUS_OVP_FOR_QC);
		bq2597x_set_busovp_alarm_th(bq, BUS_OVP_ALARM_FOR_QC);
		bq2597x_set_busocp_th(bq, BUS_OCP_FOR_QC3P5_CLASS_B);
		bq2597x_set_busocp_alarm_th(bq, BUS_OCP_ALARM_FOR_QC3P5_CLASS_B);
	} else {
		bq2597x_set_busovp_th(bq, bq->cfg->bus_ovp_th);
		bq2597x_set_busovp_alarm_th(bq, bq->cfg->bus_ovp_alm_th);
		bq2597x_set_busocp_th(bq, bq->cfg->bus_ocp_th);
		bq2597x_set_busocp_alarm_th(bq, bq->cfg->bus_ocp_alm_th);
	}
	return 0;
}

static int bq2597x_init_adc(struct bq2597x *bq)
{
	bq2597x_set_adc_scanrate(bq, false);
	bq2597x_set_adc_bits(bq, 13);
	bq2597x_set_adc_average(bq, true);
	bq2597x_set_adc_scan(bq, ADC_IBUS, true);
	bq2597x_set_adc_scan(bq, ADC_VBUS, true);
	bq2597x_set_adc_scan(bq, ADC_VOUT, false);
	bq2597x_set_adc_scan(bq, ADC_VBAT, true);
	bq2597x_set_adc_scan(bq, ADC_IBAT, true);   /* 电池侧电流，三环控制用得上 */
	bq2597x_set_adc_scan(bq, ADC_TBUS, false);
	bq2597x_set_adc_scan(bq, ADC_TBAT, false);
	/* TDIE_ADC_DIS(REG_15 bit0) 默认是 1，不清掉 tdie_raw 恒为 0。
	 * 结温降额环依赖它，必须显式使能。 */
	bq2597x_set_adc_scan(bq, ADC_TDIE, true);
	bq2597x_set_adc_scan(bq, ADC_VAC, true);

	if (bq->chip_vendor == SC8551) {
		/* improve adc accuracy (vendor sc8551_init_adc) */
		bq2597x_write_byte(bq, SC8551_REG_34, 0x01);
	}
	bq2597x_enable_adc(bq, true);

	return 0;
}

static int bq2597x_init_int_src(struct bq2597x *bq)
{
	int ret;
	/*TODO:be careful ts bus and ts bat alarm bit mask is in
	 *	fault mask register, so you need call
	 *	bq2597x_set_fault_int_mask for tsbus and tsbat alarm
	 */
	ret = bq2597x_set_alarm_int_mask(bq, ADC_DONE
					| BAT_OCP_ALARM | BAT_UCP_ALARM
					| BAT_OVP_ALARM);
	if (ret) {
		bq_err("failed to set alarm mask:%d\n", ret);
		return ret;
	}
//#if 0
	ret = bq2597x_set_fault_int_mask(bq,
			TS_BUS_FAULT | TS_DIE_FAULT | TS_BAT_FAULT | BAT_OCP_FAULT);
	if (ret) {
		bq_err("failed to set fault mask:%d\n", ret);
		return ret;
	}
//#endif
	return ret;
}

static int bq2597x_init_regulation(struct bq2597x *bq)
{
	bq2597x_set_ibat_reg_th(bq, 200);
	bq2597x_set_vbat_reg_th(bq, 50);

	bq2597x_set_vdrop_deglitch(bq, 5000);
	bq2597x_set_vdrop_th(bq, 400);

	bq2597x_enable_regulation(bq, true);

	return 0;
}

static int bq2597x_init_device(struct bq2597x *bq)
{
	int ret;
	u8 val;
	bq2597x_enable_wdt(bq, false);

	bq2597x_set_ss_timeout(bq, 1500);
	bq2597x_set_ibus_ucp_thr(bq, 300);
	bq2597x_enable_ucp(bq,1);
	bq2597x_set_sense_resistor(bq, bq->cfg->sense_r_mohm);

	bq2597x_init_protection(bq);
	bq2597x_init_adc(bq);
	bq2597x_init_int_src(bq);

	ret = bq2597x_read_byte(bq, BQ2597X_REG_13, &val);
	bq_err("Bq device ID = 0x%02X\n", val);
	if (!ret && val == BQ25968_DEV_ID) {
		bq_err("Bq device ID = 0x%02X\n", val);
		return 0;
	}

	bq2597x_init_regulation(bq);

	return 0;
}

static int bq2597x_set_present(struct bq2597x *bq, bool present)
{
	bq->usb_present = present;

	if (present)
		bq2597x_init_device(bq);
	return 0;
}

/* ====================================================================
 * elish port: minimal sysfs + probe (vendor class/psy/irq glue dropped)
 * ==================================================================== */

static ssize_t charge_enabled_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);
	bool enabled;
	int ret;

	ret = bq2597x_check_charge_enabled(bq, &enabled);
	if (ret)
		return ret;
	return sprintf(buf, "%d\n", enabled);
}

static ssize_t charge_enabled_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	ret = bq2597x_enable_charge(bq, enable);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_RW(charge_enabled);

#define BQ_SHOW_SCALED(name, ch, base, lsb, fmt)			\
static ssize_t name##_show(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
{									\
	struct i2c_client *client = to_i2c_client(dev);		\
	struct bq2597x *bq = i2c_get_clientdata(client);		\
	int v, ret;							\
	ret = bq2597x_get_adc_data(bq, ch, &v);			\
	if (ret)							\
		return ret;						\
	return sprintf(buf, fmt "\n", (v - base) * lsb);		\
}									\
static DEVICE_ATTR_RO(name)

static int bq2597x_show_val(struct bq2597x *bq, int ch)
{
	return bq2597x_adc_scaled(bq, ch);
}

#define BQ_SHOW(name, ch)						\
static ssize_t name##_show(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
{									\
	struct i2c_client *client = to_i2c_client(dev);	\
	struct bq2597x *bq = i2c_get_clientdata(client);	\
	int v = bq2597x_show_val(bq, ch);			\
	if (v < 0)						\
		return v;					\
	return sprintf(buf, "%d\n", v);				\
}									\
static DEVICE_ATTR_RO(name)

BQ_SHOW(ibus_ma, ADC_IBUS);
BQ_SHOW(vbus_mv, ADC_VBUS);
BQ_SHOW(vac_mv, ADC_VAC);
BQ_SHOW(vbat_mv, ADC_VBAT);
BQ_SHOW(tdie_raw, ADC_TDIE);

static ssize_t regs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bq2597x *bq = i2c_get_clientdata(client);
	u8 val;
	int idx = 0, ret, addr;

	for (addr = 0x00; addr <= 0x36; addr++) {
		ret = bq2597x_read_byte(bq, addr, &val);
		if (ret == 0)
			idx += sprintf(buf + idx, "%.2x:%.2x\n", addr, val);
	}
	return idx;
}
static DEVICE_ATTR_RO(regs);

static struct attribute *bq2597x_elish_attrs[] = {
	&dev_attr_charge_enabled.attr,
	&dev_attr_ibus_ma.attr,
	&dev_attr_vbus_mv.attr,
	&dev_attr_vac_mv.attr,
	&dev_attr_vbat_mv.attr,
	&dev_attr_tdie_raw.attr,
	&dev_attr_regs.attr,
	NULL
};
ATTRIBUTE_GROUPS(bq2597x_elish);

static int bq2597x_elish_probe(struct i2c_client *client)
{
	struct bq2597x *bq;
	int ret, role;

	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;

	bq->dev = &client->dev;
	bq->client = client;
	i2c_set_clientdata(client, bq);
	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->data_lock);
	mutex_init(&bq->charging_disable_lock);
	mutex_init(&bq->irq_complete);

	ret = bq2597x_detect_device(bq);
	if (ret) {
		dev_err(&client->dev, "no bq2597x device found (%d)\n", ret);
		return -ENODEV;
	}

	role = (int)(uintptr_t)of_device_get_match_data(&client->dev);
	ret = bq2597x_get_work_mode(bq, &bq->mode);
	if (ret) {
		dev_err(&client->dev, "work mode read failed\n");
		return ret;
	}

	if (bq->mode != role) {
		dev_err(&client->dev,
			"mode mismatch: chip=%d dts=%d\n", bq->mode, role);
		return -EINVAL;
	}

	ret = bq2597x_parse_dt(bq, &client->dev);
	if (ret)
		return -EIO;

	ret = bq2597x_init_device(bq);
	if (ret) {
		dev_err(&client->dev, "device init failed\n");
		return ret;
	}

	/* start disabled - the FC2 daemon enables the pump explicitly */
	bq2597x_enable_charge(bq, false);

	ret = sysfs_create_group(&client->dev.kobj, &bq2597x_elish_group);
	if (ret)
		return ret;

	dev_info(&client->dev, "bq2597x ready (id=0x%02x, mode=%d)\n",
		 bq->part_no, bq->mode);
	return 0;
}

static void bq2597x_elish_remove(struct i2c_client *client)
{
	struct bq2597x *bq = i2c_get_clientdata(client);

	bq2597x_enable_charge(bq, false);
	sysfs_remove_group(&client->dev.kobj, &bq2597x_elish_group);
}

static const struct of_device_id bq2597x_elish_match_table[] = {
	{ .compatible = "ti,bq2597x-master", .data = (void *)BQ25970_MASTER },
	{ .compatible = "ti,bq2597x-slave", .data = (void *)BQ25970_SLAVE },
	{ },
};
MODULE_DEVICE_TABLE(of, bq2597x_elish_match_table);

static struct i2c_driver bq2597x_elish_driver = {
	.driver = {
		.name = "bq2597x-elish",
		.owner = THIS_MODULE,
		.of_match_table = bq2597x_elish_match_table,
	},
	.probe = bq2597x_elish_probe,
	.remove = bq2597x_elish_remove,
};
module_i2c_driver(bq2597x_elish_driver);

MODULE_AUTHOR("Xiaomi/Qualcomm (vendor), elish port trimmed");
MODULE_DESCRIPTION("bq25970 charge pump driver for Xiaomi Pad 5 Pro (elish)");
MODULE_LICENSE("GPL");
