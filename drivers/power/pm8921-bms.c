/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 * Copyright (C) 2012 Sony Mobile Communications AB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/power_supply.h>
#include <linux/mfd/pm8xxx/pm8921-bms.h>
#include <linux/mfd/pm8xxx/core.h>
#include <linux/mfd/pm8xxx/pm8xxx-adc.h>
#include <linux/mfd/pm8xxx/ccadc.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#define BMS_CONTROL		0x224
#define BMS_S1_DELAY		0x225
#define BMS_OUTPUT0		0x230
#define BMS_OUTPUT1		0x231
#define BMS_TOLERANCES		0x232
#define BMS_TEST1		0x237

#define ADC_ARB_SECP_CNTRL	0x190
#define ADC_ARB_SECP_AMUX_CNTRL	0x191
#define ADC_ARB_SECP_ANA_PARAM	0x192
#define ADC_ARB_SECP_DIG_PARAM	0x193
#define ADC_ARB_SECP_RSV	0x194
#define ADC_ARB_SECP_DATA1	0x195
#define ADC_ARB_SECP_DATA0	0x196

#define ADC_ARB_BMS_CNTRL	0x18D
#define AMUX_TRIM_2		0x322
#define TEST_PROGRAM_REV	0x339

#define TEMP_SOC_STORAGE	0x107

enum pmic_bms_interrupts {
	PM8921_BMS_SBI_WRITE_OK,
	PM8921_BMS_CC_THR,
	PM8921_BMS_VSENSE_THR,
	PM8921_BMS_VSENSE_FOR_R,
	PM8921_BMS_OCV_FOR_R,
	PM8921_BMS_GOOD_OCV,
	PM8921_BMS_VSENSE_AVG,
	PM_BMS_MAX_INTS,
};

struct pm8921_soc_params {
	uint16_t	last_good_ocv_raw;
	int		cc;

	int		last_good_ocv_uv;
};

struct pm8921_rbatt_params {
	uint16_t	ocv_for_rbatt_raw;
	uint16_t	vsense_for_rbatt_raw;
	uint16_t	vbatt_for_rbatt_raw;

	int		ocv_for_rbatt_uv;
	int		vsense_for_rbatt_uv;
	int		vbatt_for_rbatt_uv;
};

/**
 * struct pm8921_bms_chip -
 * @bms_output_lock:	lock to prevent concurrent bms reads
 *
 * @last_ocv_uv_mutex:	mutex to protect simultaneous invocations of calculate
 *			state of charge, note that last_ocv_uv could be
 *			changed as soc is adjusted. This mutex protects
 *			simultaneous updates of last_ocv_uv as well. This mutex
 *			also protects changes to *_at_100 variables used in
 *			faking 100% SOC.
 */
struct pm8921_bms_chip {
	struct device		*dev;
	struct dentry		*dent;
	unsigned int		r_sense;
	unsigned int		i_test;
	unsigned int		v_failure;
	unsigned int		fcc;
	struct single_row_lut	*fcc_temp_lut;
	struct single_row_lut	*fcc_sf_lut;
	struct pc_temp_ocv_lut	*pc_temp_ocv_lut;
	struct sf_lut		*pc_sf_lut;
	struct sf_lut		*rbatt_sf_lut;
	struct double_row_lut	*rbatt_temp_soc_lut;
	int			delta_rbatt_mohm;
	struct work_struct	calib_hkadc_work;
	unsigned int		revision;
	unsigned int		xoadc_v0625_usb_present;
	unsigned int		xoadc_v0625_usb_absent;
	unsigned int		xoadc_v0625;
	unsigned int		xoadc_v125;
	unsigned int		batt_temp_channel;
	unsigned int		vbat_channel;
	unsigned int		ref625mv_channel;
	unsigned int		ref1p25v_channel;
	unsigned int		batt_id_channel;
	unsigned int		pmic_bms_irq[PM_BMS_MAX_INTS];
	DECLARE_BITMAP(enabled_irqs, PM_BMS_MAX_INTS);
	struct mutex		bms_output_lock;
	struct single_row_lut	*adjusted_fcc_temp_lut;
	unsigned int		charging_began;
	unsigned int		start_percent;
	unsigned int		end_percent;
	unsigned int		default_rbatt_mohms;
	enum battery_type	batt_type;
	uint16_t		ocv_reading_at_100;
	int			cc_reading_at_100;
	int			max_voltage_uv;

	int			batt_temp_suspend;
	int			soc_rbatt_suspend;
	int			default_rbatt_mohm;
	int			amux_2_trim_delta;
	uint16_t		prev_last_good_ocv_raw;
	unsigned int		rconn_mohm;
	struct mutex		last_ocv_uv_mutex;
	int			last_ocv_uv;
	int			last_cc_uah;
	struct timeval		t;
	int			last_uuc_uah;
	int			enable_fcc_learning;
	int			shutdown_soc;
	int			timer_uuc_expired;
	struct delayed_work	uuc_timer_work;
	int			uuc_uah_iavg_prev;
};

static int shutdown_soc_invalid;
static struct pm8921_bms_chip *the_chip;

#define DEFAULT_OCV_MICROVOLTS		3900000
#define DEFAULT_CHARGE_CYCLES		0
#define DEFAULT_RATIO			1000

static int last_usb_cal_delta_uv = 1800;
module_param(last_usb_cal_delta_uv, int, 0644);

static int last_chargecycles = DEFAULT_CHARGE_CYCLES;
static int last_charge_increase;
module_param(last_chargecycles, int, 0644);
module_param(last_charge_increase, int, 0644);

static int last_rbatt = -EINVAL;
static int last_soc = -EINVAL;
static int last_real_fcc_mah = -EINVAL;
static int last_real_fcc_batt_temp = -EINVAL;

static int read_cc_param;
static int read_last_good_ocv_param;
static int read_ocv_for_rbatt_param;
static int read_vbatt_for_rbatt_param;
static int read_vsense_avg_param;
static int read_vsense_for_rbatt_param;
static int read_pc_unusable_param;
static int read_ratio_for_readjust_fcc_param;
static int read_total_ratio_for_readjust_fcc_param = -EINVAL;
static int ratio_to_default_rbatt = DEFAULT_RATIO;
static int read_soc_original_param;
static int read_soc_expand_param = -EINVAL;

static int bms_ops_set(const char *val, const struct kernel_param *kp)
{
	if (*(int *)kp->arg == -EINVAL)
		return param_set_int(val, kp);
	else
		return 0;
}

static struct kernel_param_ops bms_param_ops = {
	.set = bms_ops_set,
	.get = param_get_int,
};

module_param_cb(last_rbatt, &bms_param_ops, &last_rbatt, 0644);
module_param_cb(last_soc, &bms_param_ops, &last_soc, 0644);

/*
 * bms_fake_battery is set in setups where a battery emulator is used instead
 * of a real battery. This makes the bms driver report a different/fake value
 * regardless of the calculated state of charge.
 */
static int bms_fake_battery = -EINVAL;
module_param(bms_fake_battery, int, 0644);

/* bms_start_XXX and bms_end_XXX are read only */
static int bms_start_percent;
static int bms_start_ocv_uv;
static int bms_start_cc_uah;
static int bms_end_percent;
static int bms_end_ocv_uv;
static int bms_end_cc_uah;

static int bms_ro_ops_set(const char *val, const struct kernel_param *kp)
{
	return -EINVAL;
}

static struct kernel_param_ops bms_ro_param_ops = {
	.set = bms_ro_ops_set,
	.get = param_get_int,
};
module_param_cb(bms_start_percent, &bms_ro_param_ops, &bms_start_percent, 0644);
module_param_cb(bms_start_ocv_uv, &bms_ro_param_ops, &bms_start_ocv_uv, 0644);
module_param_cb(bms_start_cc_uah, &bms_ro_param_ops, &bms_start_cc_uah, 0644);

module_param_cb(bms_end_percent, &bms_ro_param_ops, &bms_end_percent, 0644);
module_param_cb(bms_end_ocv_uv, &bms_ro_param_ops, &bms_end_ocv_uv, 0644);
module_param_cb(bms_end_cc_uah, &bms_ro_param_ops, &bms_end_cc_uah, 0644);

#define MAX_RATIO_EACH		1100
#define MIN_RATIO_EACH		900
#define MAX_RATIO_TOTAL		1100
#define MIN_RATIO_TOTAL		400
static int calculate_fcc_uah(struct pm8921_bms_chip *chip, int batt_temp,
		int chargecycles);
static void readjust_fcc_table(void)
{
	struct single_row_lut *temp, *old;
	int i, fcc_mah, ratio;

	if (!the_chip->fcc_temp_lut) {
		pr_err("The static fcc lut table is NULL\n");
		return;
	}

	temp = kzalloc(sizeof(struct single_row_lut), GFP_KERNEL);
	if (!temp) {
		pr_err("Cannot allocate memory for adjusted fcc table\n");
		return;
	}

	fcc_mah = calculate_fcc_uah(the_chip, last_real_fcc_batt_temp, 0)
		/ 1000;

	ratio = div_u64(last_real_fcc_mah * DEFAULT_RATIO, fcc_mah);
	read_ratio_for_readjust_fcc_param = ratio;
	if (ratio > MAX_RATIO_EACH)
		ratio = MAX_RATIO_EACH;
	else if (ratio < MIN_RATIO_EACH)
		ratio = MIN_RATIO_EACH;
	if (read_total_ratio_for_readjust_fcc_param == -EINVAL)
		read_total_ratio_for_readjust_fcc_param = DEFAULT_RATIO;
	ratio = read_total_ratio_for_readjust_fcc_param * ratio / DEFAULT_RATIO;
	if (ratio > MAX_RATIO_TOTAL)
		ratio = MAX_RATIO_TOTAL;
	else if (ratio < MIN_RATIO_TOTAL)
		ratio = MIN_RATIO_TOTAL;
	read_total_ratio_for_readjust_fcc_param = ratio;
	pr_info("ratio = %d\n", ratio);

	temp->cols = the_chip->fcc_temp_lut->cols;
	for (i = 0; i < the_chip->fcc_temp_lut->cols; i++) {
		temp->x[i] = the_chip->fcc_temp_lut->x[i];
		temp->y[i] = (ratio * the_chip->fcc_temp_lut->y[i]);
		temp->y[i] /= 1000;
		pr_debug("temp=%d, staticfcc=%d, adjfcc=%d, ratio=%d\n",
				temp->x[i], the_chip->fcc_temp_lut->y[i],
				temp->y[i], ratio);
	}

	old = the_chip->adjusted_fcc_temp_lut;
	the_chip->adjusted_fcc_temp_lut = temp;
	kfree(old);
}

static int bms_last_real_fcc_set(const char *val,
				const struct kernel_param *kp)
{
	int rc = 0;

	if (last_real_fcc_mah == -EINVAL)
		rc = param_set_int(val, kp);
	if (rc) {
		pr_err("Failed to set last_real_fcc_mah rc=%d\n", rc);
		return rc;
	}
	if (last_real_fcc_batt_temp != -EINVAL)
		readjust_fcc_table();
	return rc;
}
static struct kernel_param_ops bms_last_real_fcc_param_ops = {
	.set = bms_last_real_fcc_set,
	.get = param_get_int,
};
module_param_cb(last_real_fcc_mah, &bms_last_real_fcc_param_ops,
					&last_real_fcc_mah, 0644);

static int bms_last_real_fcc_batt_temp_set(const char *val,
				const struct kernel_param *kp)
{
	int rc = 0;

	if (last_real_fcc_batt_temp == -EINVAL)
		rc = param_set_int(val, kp);
	if (rc) {
		pr_err("Failed to set last_real_fcc_batt_temp rc=%d\n", rc);
		return rc;
	}
	if (last_real_fcc_mah != -EINVAL)
		readjust_fcc_table();
	return rc;
}

static struct kernel_param_ops bms_last_real_fcc_batt_temp_param_ops = {
	.set = bms_last_real_fcc_batt_temp_set,
	.get = param_get_int,
};
module_param_cb(last_real_fcc_batt_temp, &bms_last_real_fcc_batt_temp_param_ops,
					&last_real_fcc_batt_temp, 0644);

static int pm_bms_get_rt_status(struct pm8921_bms_chip *chip, int irq_id)
{
	return pm8xxx_read_irq_stat(chip->dev->parent,
					chip->pmic_bms_irq[irq_id]);
}

static void pm8921_bms_enable_irq(struct pm8921_bms_chip *chip, int interrupt)
{
	if (!__test_and_set_bit(interrupt, chip->enabled_irqs)) {
		dev_dbg(chip->dev, "%s %d\n", __func__,
						chip->pmic_bms_irq[interrupt]);
		enable_irq(chip->pmic_bms_irq[interrupt]);
	}
}

static void pm8921_bms_disable_irq(struct pm8921_bms_chip *chip, int interrupt)
{
	if (__test_and_clear_bit(interrupt, chip->enabled_irqs)) {
		pr_debug("%d\n", chip->pmic_bms_irq[interrupt]);
		disable_irq_nosync(chip->pmic_bms_irq[interrupt]);
	}
}

static int pm_bms_masked_write(struct pm8921_bms_chip *chip, u16 addr,
							u8 mask, u8 val)
{
	int rc;
	u8 reg;

	rc = pm8xxx_readb(chip->dev->parent, addr, &reg);
	if (rc) {
		pr_err("read failed addr = %03X, rc = %d\n", addr, rc);
		return rc;
	}
	reg &= ~mask;
	reg |= val & mask;
	rc = pm8xxx_writeb(chip->dev->parent, addr, reg);
	if (rc) {
		pr_err("write failed addr = %03X, rc = %d\n", addr, rc);
		return rc;
	}
	return 0;
}

static int usb_chg_plugged_in(void)
{
	union power_supply_propval ret = {0,};
	static struct power_supply *psy;

	if (psy == NULL) {
		psy = power_supply_get_by_name("usb");
		if (psy == NULL)
			return 0;
	}

	if (psy->get_property(psy, POWER_SUPPLY_PROP_ONLINE, &ret))
		return 0;

	return ret.intval;
}

#define HOLD_OREG_DATA		BIT(1)
static int pm_bms_lock_output_data(struct pm8921_bms_chip *chip)
{
	int rc;

	rc = pm_bms_masked_write(chip, BMS_CONTROL, HOLD_OREG_DATA,
					HOLD_OREG_DATA);
	if (rc) {
		pr_err("couldnt lock bms output rc = %d\n", rc);
		return rc;
	}
	return 0;
}

static int pm_bms_unlock_output_data(struct pm8921_bms_chip *chip)
{
	int rc;

	rc = pm_bms_masked_write(chip, BMS_CONTROL, HOLD_OREG_DATA, 0);
	if (rc) {
		pr_err("fail to unlock BMS_CONTROL rc = %d\n", rc);
		return rc;
	}
	return 0;
}

#define SELECT_OUTPUT_DATA	0x1C
#define SELECT_OUTPUT_TYPE_SHIFT	2
#define OCV_FOR_RBATT		0x0
#define VSENSE_FOR_RBATT	0x1
#define VBATT_FOR_RBATT		0x2
#define CC_MSB			0x3
#define CC_LSB			0x4
#define LAST_GOOD_OCV_VALUE	0x5
#define VSENSE_AVG		0x6
#define VBATT_AVG		0x7

static int pm_bms_read_output_data(struct pm8921_bms_chip *chip, int type,
						int16_t *result)
{
	int rc;
	u8 reg;

	if (!result) {
		pr_err("result pointer null\n");
		return -EINVAL;
	}
	*result = 0;
	if (type < OCV_FOR_RBATT || type > VBATT_AVG) {
		pr_err("invalid type %d asked to read\n", type);
		return -EINVAL;
	}

	rc = pm_bms_masked_write(chip, BMS_CONTROL, SELECT_OUTPUT_DATA,
					type << SELECT_OUTPUT_TYPE_SHIFT);
	if (rc) {
		pr_err("fail to select %d type in BMS_CONTROL rc = %d\n",
						type, rc);
		return rc;
	}

	rc = pm8xxx_readb(chip->dev->parent, BMS_OUTPUT0, &reg);
	if (rc) {
		pr_err("fail to read BMS_OUTPUT0 for type %d rc = %d\n",
			type, rc);
		return rc;
	}
	*result = reg;
	rc = pm8xxx_readb(chip->dev->parent, BMS_OUTPUT1, &reg);
	if (rc) {
		pr_err("fail to read BMS_OUTPUT1 for type %d rc = %d\n",
			type, rc);
		return rc;
	}
	*result |= reg << 8;
	pr_debug("type %d result %x", type, *result);
	return 0;
}

#define V_PER_BIT_MUL_FACTOR	97656
#define V_PER_BIT_DIV_FACTOR	1000
#define XOADC_INTRINSIC_OFFSET	0x6000
static int xoadc_reading_to_microvolt(unsigned int a)
{
	if (a <= XOADC_INTRINSIC_OFFSET)
		return 0;

	return (a - XOADC_INTRINSIC_OFFSET)
			* V_PER_BIT_MUL_FACTOR / V_PER_BIT_DIV_FACTOR;
}

#define XOADC_CALIB_UV		625000
#define VBATT_MUL_FACTOR	3
static int adjust_xo_vbatt_reading(struct pm8921_bms_chip *chip,
					int usb_chg, unsigned int uv)
{
	s64 numerator, denominator;
	int local_delta;

	if (uv == 0)
		return 0;

	/* dont adjust if not calibrated */
	if (chip->xoadc_v0625 == 0 || chip->xoadc_v125 == 0) {
		pr_debug("No cal yet return %d\n", VBATT_MUL_FACTOR * uv);
		return VBATT_MUL_FACTOR * uv;
	}

	if (usb_chg)
		local_delta = last_usb_cal_delta_uv;
	else
		local_delta = 0;

	pr_debug("using delta = %d\n", local_delta);
	numerator = ((s64)uv - chip->xoadc_v0625 - local_delta)
							* XOADC_CALIB_UV;
	denominator =  (s64)chip->xoadc_v125 - chip->xoadc_v0625 - local_delta;
	if (denominator == 0)
		return uv * VBATT_MUL_FACTOR;
	return (XOADC_CALIB_UV + local_delta + div_s64(numerator, denominator))
						* VBATT_MUL_FACTOR;
}

#define CC_RESOLUTION_N		868056
#define CC_RESOLUTION_D		10000

static s64 cc_to_microvolt(struct pm8921_bms_chip *chip, s64 cc)
{
	return div_s64(cc * CC_RESOLUTION_N, CC_RESOLUTION_D);
}

#define CC_READING_TICKS	55
#define SLEEP_CLK_HZ		32768
#define SECONDS_PER_HOUR	3600
/**
 * ccmicrovolt_to_nvh -
 * @cc_uv:  coulumb counter converted to uV
 *
 * RETURNS:	coulumb counter based charge in nVh
 *		(nano Volt Hour)
 */
static s64 ccmicrovolt_to_nvh(s64 cc_uv)
{
	return div_s64(cc_uv * CC_READING_TICKS * 1000,
			SLEEP_CLK_HZ * SECONDS_PER_HOUR);
}

/* returns the signed value read from the hardware */
static int read_cc(struct pm8921_bms_chip *chip, int *result)
{
	int rc;
	uint16_t msw, lsw;

	rc = pm_bms_read_output_data(chip, CC_LSB, &lsw);
	if (rc) {
		pr_err("fail to read CC_LSB rc = %d\n", rc);
		return rc;
	}
	rc = pm_bms_read_output_data(chip, CC_MSB, &msw);
	if (rc) {
		pr_err("fail to read CC_MSB rc = %d\n", rc);
		return rc;
	}
	*result = msw << 16 | lsw;
	pr_debug("msw = %04x lsw = %04x cc = %d\n", msw, lsw, *result);
	read_cc_param = *result;
	return 0;
}

static int adjust_xo_vbatt_reading_for_mbg(struct pm8921_bms_chip *chip,
						int result)
{
	int64_t numerator;
	int64_t denominator;

	if (chip->amux_2_trim_delta == 0)
		return result;

	numerator = (s64)result * 1000000;
	denominator = (1000000 + (410 * (s64)chip->amux_2_trim_delta));
	return div_s64(numerator, denominator);
}

static int convert_vbatt_raw_to_uv(struct pm8921_bms_chip *chip,
					int usb_chg,
					uint16_t reading, int *result)
{
	*result = xoadc_reading_to_microvolt(reading);
	pr_debug("raw = %04x vbatt = %u\n", reading, *result);
	*result = adjust_xo_vbatt_reading(chip, usb_chg, *result);
	pr_debug("after adj vbatt = %u\n", *result);
	*result = adjust_xo_vbatt_reading_for_mbg(chip, *result);
	return 0;
}

static int convert_vsense_to_uv(struct pm8921_bms_chip *chip,
					int16_t reading, int *result)
{
	*result = pm8xxx_ccadc_reading_to_microvolt(chip->revision, reading);
	pr_debug("raw = %04x vsense = %d\n", reading, *result);
	*result = pm8xxx_cc_adjust_for_gain(*result);
	pr_debug("after adj vsense = %d\n", *result);
	return 0;
}

static int read_vsense_avg(struct pm8921_bms_chip *chip, int *result)
{
	int rc;
	int16_t reading;

	rc = pm_bms_read_output_data(chip, VSENSE_AVG, &reading);
	if (rc) {
		pr_err("fail to read VSENSE_AVG rc = %d\n", rc);
		return rc;
	}

	convert_vsense_to_uv(chip, reading, result);
	read_vsense_avg_param = *result;
	return 0;
}

static int linear_interpolate(int y0, int x0, int y1, int x1, int x)
{
	if (y0 == y1 || x == x0)
		return y0;
	if (x1 == x0 || x == x1)
		return y1;

	return y0 + ((y1 - y0) * (x - x0) / (x1 - x0));
}

static int interpolate_single_lut(struct single_row_lut *lut, int x)
{
	int i, result;

	if (x < lut->x[0]) {
		pr_debug("x %d less than known range return y = %d lut = %pS\n",
							x, lut->y[0], lut);
		return lut->y[0];
	}
	if (x > lut->x[lut->cols - 1]) {
		pr_debug("x %d more than known range return y = %d lut = %pS\n",
						x, lut->y[lut->cols - 1], lut);
		return lut->y[lut->cols - 1];
	}

	for (i = 0; i < lut->cols; i++)
		if (x <= lut->x[i])
			break;
	if (x == lut->x[i]) {
		result = lut->y[i];
	} else {
		result = linear_interpolate(
			lut->y[i - 1],
			lut->x[i - 1],
			lut->y[i],
			lut->x[i],
			x);
	}
	return result;
}

static int interpolate_fcc(struct pm8921_bms_chip *chip, int batt_temp)
{
	/* batt_temp is in tenths of degC - convert it to degC for lookups */
	batt_temp = batt_temp/10;
	return interpolate_single_lut(chip->fcc_temp_lut, batt_temp);
}

static int interpolate_fcc_adjusted(struct pm8921_bms_chip *chip, int batt_temp)
{
	/* batt_temp is in tenths of degC - convert it to degC for lookups */
	batt_temp = batt_temp/10;
	return interpolate_single_lut(chip->adjusted_fcc_temp_lut, batt_temp);
}

static int interpolate_scalingfactor_fcc(struct pm8921_bms_chip *chip,
								int cycles)
{
	/*
	 * sf table could be null when no battery aging data is available, in
	 * that case return 100%
	 */
	if (chip->fcc_sf_lut)
		return interpolate_single_lut(chip->fcc_sf_lut, cycles);
	else
		return 100;
}

static int interpolate_scalingfactor(struct pm8921_bms_chip *chip,
				struct sf_lut *sf_lut,
				int row_entry, int pc)
{
	int i, scalefactorrow1, scalefactorrow2, scalefactor;
	int rows, cols;
	int row1 = 0;
	int row2 = 0;

	/*
	 * sf table could be null when no battery aging data is available, in
	 * that case return 100%
	 */
	if (!sf_lut)
		return 100;

	rows = sf_lut->rows;
	cols = sf_lut->cols;
	if (pc > sf_lut->percent[0]) {
		pr_debug("pc %d greater than known pc ranges for sfd\n", pc);
		row1 = 0;
		row2 = 0;
	}
	if (pc < sf_lut->percent[rows - 1]) {
		pr_debug("pc %d less than known pc ranges for sf", pc);
		row1 = rows - 1;
		row2 = rows - 1;
	}
	for (i = 0; i < rows; i++) {
		if (pc == sf_lut->percent[i]) {
			row1 = i;
			row2 = i;
			break;
		}
		if (pc > sf_lut->percent[i]) {
			row1 = i - 1;
			row2 = i;
			break;
		}
	}

	if (row_entry < sf_lut->row_entries[0])
		row_entry = sf_lut->row_entries[0];
	if (row_entry > sf_lut->row_entries[cols - 1])
		row_entry = sf_lut->row_entries[cols - 1];

	for (i = 0; i < cols; i++)
		if (row_entry <= sf_lut->row_entries[i])
			break;
	if (row_entry == sf_lut->row_entries[i]) {
		scalefactor = linear_interpolate(
				sf_lut->sf[row1][i],
				sf_lut->percent[row1],
				sf_lut->sf[row2][i],
				sf_lut->percent[row2],
				pc);
		return scalefactor;
	}

	scalefactorrow1 = linear_interpolate(
				sf_lut->sf[row1][i - 1],
				sf_lut->row_entries[i - 1],
				sf_lut->sf[row1][i],
				sf_lut->row_entries[i],
				row_entry);

	scalefactorrow2 = linear_interpolate(
				sf_lut->sf[row2][i - 1],
				sf_lut->row_entries[i - 1],
				sf_lut->sf[row2][i],
				sf_lut->row_entries[i],
				row_entry);

	scalefactor = linear_interpolate(
				scalefactorrow1,
				sf_lut->percent[row1],
				scalefactorrow2,
				sf_lut->percent[row2],
				pc);

	return scalefactor;
}

static int is_between(int left, int right, int value)
{
	if (left >= right && left >= value && value >= right)
		return 1;
	if (left <= right && left <= value && value <= right)
		return 1;

	return 0;
}

static int interpolate_pc(struct pm8921_bms_chip *chip,
				int batt_temp, int ocv)
{
	int i, j, pcj, pcj_minus_one, pc;
	int rows = chip->pc_temp_ocv_lut->rows;
	int cols = chip->pc_temp_ocv_lut->cols;

	/* batt_temp is in tenths of degC - convert it to degC for lookups */
	batt_temp = batt_temp/10;

	if (batt_temp < chip->pc_temp_ocv_lut->temp[0]) {
		pr_debug("batt_temp %d < known temp range for pc\n", batt_temp);
		batt_temp = chip->pc_temp_ocv_lut->temp[0];
	}
	if (batt_temp > chip->pc_temp_ocv_lut->temp[cols - 1]) {
		pr_debug("batt_temp %d > known temp range for pc\n", batt_temp);
		batt_temp = chip->pc_temp_ocv_lut->temp[cols - 1];
	}

	for (j = 0; j < cols; j++)
		if (batt_temp <= chip->pc_temp_ocv_lut->temp[j])
			break;
	if (batt_temp == chip->pc_temp_ocv_lut->temp[j]) {
		/* found an exact match for temp in the table */
		if (ocv >= chip->pc_temp_ocv_lut->ocv[0][j])
			return chip->pc_temp_ocv_lut->percent[0];
		if (ocv <= chip->pc_temp_ocv_lut->ocv[rows - 1][j])
			return chip->pc_temp_ocv_lut->percent[rows - 1];
		for (i = 0; i < rows; i++) {
			if (ocv >= chip->pc_temp_ocv_lut->ocv[i][j]) {
				if (ocv == chip->pc_temp_ocv_lut->ocv[i][j])
					return
					chip->pc_temp_ocv_lut->percent[i];
				pc = linear_interpolate(
					chip->pc_temp_ocv_lut->percent[i],
					chip->pc_temp_ocv_lut->ocv[i][j],
					chip->pc_temp_ocv_lut->percent[i - 1],
					chip->pc_temp_ocv_lut->ocv[i - 1][j],
					ocv);
				return pc;
			}
		}
	}

	/*
	 * batt_temp is within temperature for
	 * column j-1 and j
	 */
	if (ocv >= chip->pc_temp_ocv_lut->ocv[0][j])
		return chip->pc_temp_ocv_lut->percent[0];
	if (ocv <= chip->pc_temp_ocv_lut->ocv[rows - 1][j - 1])
		return chip->pc_temp_ocv_lut->percent[rows - 1];

	pcj_minus_one = 0;
	pcj = 0;
	for (i = 0; i < rows-1; i++) {
		if (pcj == 0
			&& is_between(chip->pc_temp_ocv_lut->ocv[i][j],
				chip->pc_temp_ocv_lut->ocv[i+1][j], ocv)) {
			pcj = linear_interpolate(
				chip->pc_temp_ocv_lut->percent[i],
				chip->pc_temp_ocv_lut->ocv[i][j],
				chip->pc_temp_ocv_lut->percent[i + 1],
				chip->pc_temp_ocv_lut->ocv[i+1][j],
				ocv);
		}

		if (pcj_minus_one == 0
			&& is_between(chip->pc_temp_ocv_lut->ocv[i][j-1],
				chip->pc_temp_ocv_lut->ocv[i+1][j-1], ocv)) {

			pcj_minus_one = linear_interpolate(
				chip->pc_temp_ocv_lut->percent[i],
				chip->pc_temp_ocv_lut->ocv[i][j-1],
				chip->pc_temp_ocv_lut->percent[i + 1],
				chip->pc_temp_ocv_lut->ocv[i+1][j-1],
				ocv);
		}

		if (pcj && pcj_minus_one) {
			pc = linear_interpolate(
				pcj_minus_one,
				chip->pc_temp_ocv_lut->temp[j-1],
				pcj,
				chip->pc_temp_ocv_lut->temp[j],
				batt_temp);
			return pc;
		}
	}

	if (pcj)
		return pcj;

	if (pcj_minus_one)
		return pcj_minus_one;

	pr_debug("%d ocv wasn't found for temp %d in the LUT returning 100%%",
							ocv, batt_temp);
	return 100;
}

static int interpolate_double_lut(struct double_row_lut *lut,
		int in_1, int in_2)
{
	int i, j, out, out_j = 0, out_j_minus_one = 0;
	int rows = lut->rows;
	int cols = lut->cols;

	pr_debug("in_1 = %d, in_2 = %d\n", in_1, in_2);
	if (in_1 < lut->in_1[0])
		in_1 = lut->in_1[0];
	else if (in_1 > lut->in_1[cols - 1])
		in_1 = lut->in_1[cols - 1];

	for (j = 0; j < cols; j++)
		if (in_1 <= lut->in_1[j])
			break;
	if (in_1 == lut->in_1[j]) {
		/* found an exact match for in_1 */
		if (in_2 >= lut->in_2[0][j])
			return lut->out[0];
		if (in_2 <= lut->in_2[rows - 1][j])
			return lut->out[rows - 1];
		for (i = 0; i < rows; i++) {
			if (in_2 >= lut->in_2[i][j]) {
				if (in_2 == lut->in_2[i][j])
					/* found an exact match for in_2 */
					return lut->out[i];
				out = linear_interpolate(
						lut->out[i],
						lut->in_2[i][j],
						lut->out[i - 1],
						lut->in_2[i - 1][j],
						in_2);
				return out;
			}
		}
	}

	/* in_1 is within column j-1 and j */
	for (i = 0; i < rows-1; i++) {
		if (out_j == 0 && is_between(lut->in_2[i][j],
					lut->in_2[i+1][j], in_2)) {
			/* in_2 for column j is within row i and i+1 */
			out_j = linear_interpolate(
					lut->out[i],
					lut->in_2[i][j],
					lut->out[i + 1],
					lut->in_2[i+1][j],
					in_2);
			pr_debug("out_j = %d\n", out_j);
		}

		if (out_j_minus_one == 0
				&& is_between(lut->in_2[i][j-1],
					lut->in_2[i+1][j-1], in_2)) {
			/* in_2 for column j-1 is within row i and i+1 */
			out_j_minus_one = linear_interpolate(
					lut->out[i],
					lut->in_2[i][j-1],
					lut->out[i + 1],
					lut->in_2[i+1][j-1],
					in_2);
			pr_debug("out_j_minus_one = %d\n", out_j_minus_one);
		}

		if (out_j && out_j_minus_one) {
			out = linear_interpolate(
					out_j_minus_one,
					lut->in_1[j-1],
					out_j,
					lut->in_1[j],
					in_1);
			pr_debug("out = %d\n", out);
			return out;
		}
	}

	if (out_j)
		return out_j;

	if (out_j_minus_one)
		return out_j_minus_one;

	pr_debug("in_2 %d wasn't found for in_1 %d in the LUT\n", in_2, in_1);
	return lut->out[0];
}

#define BMS_MODE_BIT	BIT(6)
#define EN_VBAT_BIT	BIT(5)
#define OVERRIDE_MODE_DELAY_MS	20
int pm8921_bms_get_simultaneous_battery_voltage_and_current(int *ibat_ua,
								int *vbat_uv)
{
	int16_t vsense_raw;
	int16_t vbat_raw;
	int vsense_uv;
	int usb_chg;

	if (the_chip == NULL) {
		pr_err("Called to early\n");
		return -EINVAL;
	}

	mutex_lock(&the_chip->bms_output_lock);

	pm8xxx_writeb(the_chip->dev->parent, BMS_S1_DELAY, 0x00);
	pm_bms_masked_write(the_chip, BMS_CONTROL,
			BMS_MODE_BIT | EN_VBAT_BIT, BMS_MODE_BIT | EN_VBAT_BIT);

	msleep(OVERRIDE_MODE_DELAY_MS);

	pm_bms_lock_output_data(the_chip);
	pm_bms_read_output_data(the_chip, VSENSE_AVG, &vsense_raw);
	pm_bms_read_output_data(the_chip, VBATT_AVG, &vbat_raw);
	pm_bms_unlock_output_data(the_chip);
	pm_bms_masked_write(the_chip, BMS_CONTROL,
			BMS_MODE_BIT | EN_VBAT_BIT, 0);

	pm8xxx_writeb(the_chip->dev->parent, BMS_S1_DELAY, 0x0B);

	mutex_unlock(&the_chip->bms_output_lock);

	usb_chg = usb_chg_plugged_in();

	convert_vbatt_raw_to_uv(the_chip, usb_chg, vbat_raw, vbat_uv);
	convert_vsense_to_uv(the_chip, vsense_raw, &vsense_uv);
	*ibat_ua = vsense_uv * 1000 / (int)the_chip->r_sense;

	pr_debug("vsense_raw = 0x%x vbat_raw = 0x%x"
			" ibat_ua = %d vbat_uv = %d\n",
			(uint16_t)vsense_raw, (uint16_t)vbat_raw,
			*ibat_ua, *vbat_uv);
	return 0;
}
EXPORT_SYMBOL(pm8921_bms_get_simultaneous_battery_voltage_and_current);

static int read_rbatt_params_raw(struct pm8921_bms_chip *chip,
				struct pm8921_rbatt_params *raw)
{
	int usb_chg;

	mutex_lock(&chip->bms_output_lock);
	pm_bms_lock_output_data(chip);

	pm_bms_read_output_data(chip,
			OCV_FOR_RBATT, &raw->ocv_for_rbatt_raw);
	pm_bms_read_output_data(chip,
			VBATT_FOR_RBATT, &raw->vbatt_for_rbatt_raw);
	pm_bms_read_output_data(chip,
			VSENSE_FOR_RBATT, &raw->vsense_for_rbatt_raw);

	pm_bms_unlock_output_data(chip);
	mutex_unlock(&chip->bms_output_lock);

	usb_chg = usb_chg_plugged_in();
	convert_vbatt_raw_to_uv(chip, usb_chg,
			raw->vbatt_for_rbatt_raw, &raw->vbatt_for_rbatt_uv);
	convert_vbatt_raw_to_uv(chip, usb_chg,
			raw->ocv_for_rbatt_raw, &raw->ocv_for_rbatt_uv);
	convert_vsense_to_uv(chip, raw->vsense_for_rbatt_raw,
					&raw->vsense_for_rbatt_uv);

	pr_debug("vbatt_for_rbatt_raw = 0x%x, vbatt_for_rbatt= %duV\n",
			raw->vbatt_for_rbatt_raw, raw->vbatt_for_rbatt_uv);
	pr_debug("ocv_for_rbatt_raw = 0x%x, ocv_for_rbatt= %duV\n",
			raw->ocv_for_rbatt_raw, raw->ocv_for_rbatt_uv);
	pr_debug("vsense_for_rbatt_raw = 0x%x, vsense_for_rbatt= %duV\n",
			raw->vsense_for_rbatt_raw, raw->vsense_for_rbatt_uv);
	return 0;
}

#define MBG_TRANSIENT_ERROR_RAW 51
static void adjust_pon_ocv_raw(struct pm8921_bms_chip *chip,
				struct pm8921_soc_params *raw)
{
	/* in 8921 parts the PON ocv is taken when the MBG is not settled.
	 * decrease the pon ocv by 15mV raw value to account for it
	 * Since a 1/3rd  of vbatt is supplied to the adc the raw value
	 * needs to be adjusted by 5mV worth bits
	 */
	if (raw->last_good_ocv_raw >= MBG_TRANSIENT_ERROR_RAW)
		raw->last_good_ocv_raw -= MBG_TRANSIENT_ERROR_RAW;
}

static int read_soc_params_raw(struct pm8921_bms_chip *chip,
				struct pm8921_soc_params *raw)
{
	int usb_chg;

	mutex_lock(&chip->bms_output_lock);
	pm_bms_lock_output_data(chip);

	pm_bms_read_output_data(chip,
			LAST_GOOD_OCV_VALUE, &raw->last_good_ocv_raw);
	read_cc(chip, &raw->cc);

	pm_bms_unlock_output_data(chip);
	mutex_unlock(&chip->bms_output_lock);

	usb_chg =  usb_chg_plugged_in();

	if (chip->prev_last_good_ocv_raw == 0) {
		chip->prev_last_good_ocv_raw = raw->last_good_ocv_raw;
		adjust_pon_ocv_raw(chip, raw);
		convert_vbatt_raw_to_uv(chip, usb_chg,
			raw->last_good_ocv_raw, &raw->last_good_ocv_uv);
		chip->last_ocv_uv = raw->last_good_ocv_uv;
	} else if (chip->prev_last_good_ocv_raw != raw->last_good_ocv_raw) {
		chip->prev_last_good_ocv_raw = raw->last_good_ocv_raw;
		convert_vbatt_raw_to_uv(chip, usb_chg,
			raw->last_good_ocv_raw, &raw->last_good_ocv_uv);
		chip->last_ocv_uv = raw->last_good_ocv_uv;
		/* forget the old cc value upon ocv */
		chip->last_cc_uah = 0;
	} else {
		raw->last_good_ocv_uv = chip->last_ocv_uv;
	}

	/* fake a high OCV if we are just done charging */
	if (chip->ocv_reading_at_100 != raw->last_good_ocv_raw) {
		chip->ocv_reading_at_100 = 0;
		chip->cc_reading_at_100 = 0;
	} else {
		/*
		 * force 100% ocv by selecting the highest voltage the
		 * battery could ever reach
		 */
		raw->last_good_ocv_uv = chip->max_voltage_uv;
		chip->last_ocv_uv = chip->max_voltage_uv;
	}
	pr_debug("0p625 = %duV\n", chip->xoadc_v0625);
	pr_debug("1p25 = %duV\n", chip->xoadc_v125);
	pr_debug("last_good_ocv_raw= 0x%x, last_good_ocv_uv= %duV\n",
			raw->last_good_ocv_raw, raw->last_good_ocv_uv);
	pr_debug("cc_raw= 0x%x\n", raw->cc);

	read_last_good_ocv_param = raw->last_good_ocv_uv;
	read_ocv_for_rbatt_param = raw->ocv_for_rbatt_uv;
	read_vbatt_for_rbatt_param = raw->vbatt_for_rbatt_uv;
	read_vsense_for_rbatt_param = raw->vsense_for_rbatt_uv;
	return 0;
}

static int get_rbatt(struct pm8921_bms_chip *chip, int soc_rbatt, int batt_temp)
{
	int rbatt, scalefactor;

	rbatt = (last_rbatt < 0) ? chip->default_rbatt_mohm : last_rbatt;
	pr_debug("rbatt before scaling = %d\n", rbatt);
	if (chip->rbatt_sf_lut == NULL)  {
		pr_debug("RBATT = %d\n", rbatt);
		return rbatt;
	}
	/* Convert the batt_temp to DegC from deciDegC */
	batt_temp = batt_temp / 10;
	scalefactor = interpolate_scalingfactor(chip, chip->rbatt_sf_lut,
							batt_temp, soc_rbatt);
	pr_debug("rbatt sf = %d for batt_temp = %d, soc_rbatt = %d\n",
				scalefactor, batt_temp, soc_rbatt);
	rbatt = (rbatt * scalefactor) / 100;

	rbatt += the_chip->rconn_mohm;
	pr_debug("adding rconn_mohm = %d rbatt = %d\n",
				the_chip->rconn_mohm, rbatt);

	if (is_between(20, 10, soc_rbatt))
		rbatt = rbatt
			+ ((20 - soc_rbatt) * chip->delta_rbatt_mohm) / 10;
	else
		if (is_between(10, 0, soc_rbatt))
			rbatt = rbatt + chip->delta_rbatt_mohm;

	pr_debug("RBATT = %d\n", rbatt);
	return rbatt;
}

static int calculate_rbatt_resume(struct pm8921_bms_chip *chip,
				struct pm8921_rbatt_params *raw)
{
	unsigned int  r_batt;

	if (raw->ocv_for_rbatt_uv <= 0
		|| raw->ocv_for_rbatt_uv <= raw->vbatt_for_rbatt_uv
		|| raw->vsense_for_rbatt_raw <= 0) {
		pr_debug("rbatt readings unavailable ocv = %d, vbatt = %d,"
					"vsen = %d\n",
					raw->ocv_for_rbatt_uv,
					raw->vbatt_for_rbatt_uv,
					raw->vsense_for_rbatt_raw);
		return -EINVAL;
	}
	r_batt = ((raw->ocv_for_rbatt_uv - raw->vbatt_for_rbatt_uv)
			* chip->r_sense) / raw->vsense_for_rbatt_uv;
	pr_debug("r_batt = %umilliOhms", r_batt);
	return r_batt;
}

static int calculate_fcc_uah(struct pm8921_bms_chip *chip, int batt_temp,
							int chargecycles)
{
	int initfcc, result, scalefactor = 0;

	if (chip->adjusted_fcc_temp_lut == NULL) {
		initfcc = interpolate_fcc(chip, batt_temp);

		scalefactor = interpolate_scalingfactor_fcc(chip, chargecycles);

		/* Multiply the initial FCC value by the scale factor. */
		result = (initfcc * scalefactor * 1000) / 100;
		pr_debug("fcc = %d uAh\n", result);
		return result;
	} else {
		return 1000 * interpolate_fcc_adjusted(chip, batt_temp);
	}
}

static int get_battery_uvolts(struct pm8921_bms_chip *chip, int *uvolts)
{
	int rc;
	struct pm8xxx_adc_chan_result result;

	rc = pm8xxx_adc_read(chip->vbat_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					chip->vbat_channel, rc);
		return rc;
	}
	pr_debug("mvolts phy = %lld meas = 0x%llx", result.physical,
						result.measurement);
	*uvolts = (int)result.physical;
	return 0;
}

static int adc_based_ocv(struct pm8921_bms_chip *chip, int *ocv)
{
	int vbatt, rbatt, ibatt_ua, rc;

	rc = get_battery_uvolts(chip, &vbatt);
	if (rc) {
		pr_err("failed to read vbatt from adc rc = %d\n", rc);
		return rc;
	}

	rc =  pm8921_bms_get_battery_current(&ibatt_ua);
	if (rc) {
		pr_err("failed to read batt current rc = %d\n", rc);
		return rc;
	}

	rbatt = (last_rbatt < 0) ? chip->default_rbatt_mohm : last_rbatt;
	*ocv = vbatt + (ibatt_ua * rbatt)/1000;
	return 0;
}

static int calculate_pc(struct pm8921_bms_chip *chip, int ocv_uv, int batt_temp,
							int chargecycles)
{
	int pc, scalefactor;

	pc = interpolate_pc(chip, batt_temp, ocv_uv / 1000);
	pr_debug("pc = %u for ocv = %dmicroVolts batt_temp = %d\n",
					pc, ocv_uv, batt_temp);

	scalefactor = interpolate_scalingfactor(chip,
					chip->pc_sf_lut, chargecycles, pc);
	pr_debug("scalefactor = %u batt_temp = %d\n", scalefactor, batt_temp);

	/* Multiply the initial FCC value by the scale factor. */
	pc = (pc * scalefactor) / 100;
	return pc;
}

/**
 * calculate_cc_uah -
 * @chip:		the bms chip pointer
 * @cc:			the cc reading from bms h/w
 * @val:		return value
 * @coulumb_counter:	adjusted coulumb counter for 100%
 *
 * RETURNS: in val pointer coulumb counter based charger in uAh
 *          (micro Amp hour)
 */
static void calculate_cc_uah(struct pm8921_bms_chip *chip, int cc, int *val)
{
	int64_t cc_voltage_uv, cc_nvh, cc_uah;

	cc_voltage_uv = cc;
	cc_voltage_uv -= chip->cc_reading_at_100;
	pr_debug("cc = %d. after subtracting 0x%x cc = %lld\n",
					cc, chip->cc_reading_at_100,
					cc_voltage_uv);
	cc_voltage_uv = cc_to_microvolt(chip, cc_voltage_uv);
	cc_voltage_uv = pm8xxx_cc_adjust_for_gain(cc_voltage_uv);
	pr_debug("cc_voltage_uv = %lld microvolts\n", cc_voltage_uv);
	cc_nvh = ccmicrovolt_to_nvh(cc_voltage_uv);
	pr_debug("cc_nvh = %lld nano_volt_hour\n", cc_nvh);
	cc_uah = div_s64(cc_nvh, chip->r_sense);
	*val = cc_uah;
}

static int calculate_uuc_uah_at_given_current(struct pm8921_bms_chip *chip,
				 int batt_temp, int chargecycles,
				int rbatt, int fcc_uah, int i_ma)
{
	int unusable_uv, pc_unusable, uuc;

	/* calculate unusable charge with itest */
	unusable_uv = (rbatt * i_ma) + (chip->v_failure * 1000);
	pc_unusable = calculate_pc(chip, unusable_uv, batt_temp, chargecycles);
	uuc = (fcc_uah * pc_unusable) / 100;
	pr_debug("For i_ma = %d, unusable_uv = %d unusable_pc = %d uuc = %d\n",
					i_ma, unusable_uv, pc_unusable, uuc);
	return uuc;
}

#define SOC_RBATT_CHG		70
#define SOC_RBATT_DISCHG	20

static int uuc_iavg_div = 150;
module_param(uuc_iavg_div, int, 0644);

static int uuc_min_step_size = 120;
module_param(uuc_min_step_size, int, 0644);

static int uuc_multiplier = 1000;
module_param(uuc_multiplier, int, 0644);

#define UUC_TIMER_MS		120000

static void uuc_timer_work(struct work_struct *work)
{
	struct pm8921_bms_chip *chip = container_of(work,
				struct pm8921_bms_chip, uuc_timer_work.work);

	pr_debug("UUC Timer expired\n");
	/* indicates the system is done with the high load during bootup */
	chip->timer_uuc_expired = 1;
}

static void calculate_iavg_ua(struct pm8921_bms_chip *chip, int cc_uah,
				int *iavg_ua, int *delta_time_us)
{
	int delta_cc_uah;
	struct timeval now;

	delta_cc_uah = cc_uah - chip->last_cc_uah;
	do_gettimeofday(&now);
	if (chip->t.tv_sec != 0) {
		*delta_time_us = (now.tv_sec - chip->t.tv_sec) * USEC_PER_SEC
				+ now.tv_usec - chip->t.tv_usec;
	} else {
		/* calculation for the first time */
		*delta_time_us = 0;
	}

	if (*delta_time_us != 0)
		*iavg_ua = div_s64((s64)delta_cc_uah * 3600 * 1000000,
					*delta_time_us);
	else
		*iavg_ua = 0;

	pr_debug("t.tv_sec = %d, now.tv_sec = %d delta_us = %d iavg_ua = %d\n",
				(int)chip->t.tv_sec, (int)now.tv_sec,
				*delta_time_us, (int)*iavg_ua);
	/* remember cc_uah */
	chip->last_cc_uah = cc_uah;

	/* remember this time */
	chip->t = now;
}

#define UUC_IAVG_THRESHOLD_UAH	50000
static int scale_unusable_charge_uah(struct pm8921_bms_chip *chip,
			bool charging, int uuc_uah_iavg, int uuc_uah_itest,
			int uuc_uah_iavg_prev)
{
	int stepsize = 0;
	int delta_uuc = 0;
	int uuc_reported = 0;

	if (charging) {
		stepsize = max(uuc_min_step_size,
				uuc_multiplier * (SOC_RBATT_CHG - last_soc));
		/*
		 * set the delta only if uuc is decreasing. If it has increased
		 * simply report the last uuc since we don't want to report a
		 * higher uuc as charging progresses
		 */
		if (chip->last_uuc_uah > uuc_uah_iavg)
			delta_uuc = (chip->last_uuc_uah - uuc_uah_iavg)
								/ stepsize;
		uuc_reported = chip->last_uuc_uah - delta_uuc;
	} else {
		stepsize = max(uuc_min_step_size,
			uuc_multiplier * (last_soc - SOC_RBATT_DISCHG));
		if (uuc_uah_itest > uuc_uah_iavg) {
			if ((uuc_uah_iavg > uuc_uah_iavg_prev
						+ UUC_IAVG_THRESHOLD_UAH)
				&& chip->timer_uuc_expired)
				/*
				 * there is a big jump in iavg current way past
				 * the bootup increase  uuc to this high iavg
				 * based uuc in steps
				 */
				delta_uuc = (uuc_uah_iavg - uuc_uah_iavg_prev)
							/ uuc_iavg_div;
			else
				/* increase uuc towards itest based uuc */
				delta_uuc = (uuc_uah_itest - uuc_uah_iavg)
						/ stepsize;
		} else {
			/*
			 * the iavg based uuc was higher than itest based
			 * uuc. This means that iavg > itest. Itest represents
			 * the max current drawn from the device at anytime.
			 * If we find iavg > itest, ignore iavg and simply step
			 * up the uuc based on itest
			 */
			delta_uuc = uuc_uah_itest / stepsize;
		}
		uuc_reported = min(uuc_uah_itest,
					chip->last_uuc_uah + delta_uuc);
	}
	pr_debug("uuc_prev = %d stepsize = %d d_uuc =  %d uuc_reported = %d\n",
			chip->last_uuc_uah, (int)stepsize, delta_uuc,
			uuc_reported);
	return uuc_reported;
}

static void readjust_rbatt_table(int rbatt, int rbatt_from_lut)
{
	ratio_to_default_rbatt = div_u64(rbatt * DEFAULT_RATIO, rbatt_from_lut);
	pr_info("ratio_to_default_rbatt = %d\n", ratio_to_default_rbatt);
}

static int calculate_unusable_charge_uah(struct pm8921_bms_chip *chip,
				int rbatt, int fcc_uah, int cc_uah,
				int soc_rbatt, int batt_temp, int chargecycles,
				int iavg_ua)
{
	int uuc_uah_itest, uuc_uah_iavg, uuc_reported;
	static int firsttime = 1;
	int iavg_ma = iavg_ua / 1000;

	/* calculate unusable charge with itest */
	uuc_uah_itest = calculate_uuc_uah_at_given_current(chip,
					batt_temp, chargecycles,
					rbatt, fcc_uah, chip->i_test);

	pr_debug("itest = %d uuc_itest = %d\n", chip->i_test, uuc_uah_itest);

	/* calculate unusable charge with iavg */
	iavg_ma = max(0, iavg_ma);
	uuc_uah_iavg = calculate_uuc_uah_at_given_current(chip,
					batt_temp, chargecycles,
					rbatt, fcc_uah, iavg_ma);
	pr_debug("iavg = %d uuc_iavg = %d\n", iavg_ma, uuc_uah_iavg);

	if (firsttime) {
		chip->uuc_uah_iavg_prev = uuc_uah_iavg;

		if (cc_uah < chip->last_cc_uah)
			chip->last_uuc_uah = uuc_uah_itest;
		else
			chip->last_uuc_uah = uuc_uah_iavg;
		pr_debug("firsttime uuc_prev = %d\n", chip->last_uuc_uah);
	}

	uuc_reported = scale_unusable_charge_uah(chip,
				cc_uah < chip->last_cc_uah,
				uuc_uah_iavg, uuc_uah_itest,
				chip->uuc_uah_iavg_prev);

	/* remember the last uuc_uah_iavg */
	chip->uuc_uah_iavg_prev = uuc_uah_iavg;

	/* remember the reported uuc */
	chip->last_uuc_uah = uuc_reported;

	if (firsttime == 1) {
		/* uuc calculation for the first time is done */
		firsttime = 0;
	}

	read_pc_unusable_param = pc_unusable;
	return uuc_reported;
}

/* calculate remainging charge at the time of ocv */
static int calculate_remaining_charge_uah(struct pm8921_bms_chip *chip,
						struct pm8921_soc_params *raw,
						int fcc_uah, int batt_temp,
						int chargecycles)
{
	int  ocv, pc;

	ocv = raw->last_good_ocv_uv;
	pc = calculate_pc(chip, ocv, batt_temp, chargecycles);
	pr_debug("ocv = %d pc = %d\n", ocv, pc);
	return (fcc_uah * pc) / 100;
}

static void calculate_soc_params(struct pm8921_bms_chip *chip,
						struct pm8921_soc_params *raw,
						int batt_temp, int chargecycles,
						int *fcc_uah,
						int *unusable_charge_uah,
						int *remaining_charge_uah,
						int *cc_uah,
						int *rbatt,
						int *iavg_ua,
						int *delta_time_us)
{
	int soc_rbatt;

	*fcc_uah = calculate_fcc_uah(chip, batt_temp, chargecycles);
	pr_debug("FCC = %uuAh batt_temp = %d, cycles = %d\n",
					*fcc_uah, batt_temp, chargecycles);


	/* calculate remainging charge */
	*remaining_charge_uah = calculate_remaining_charge_uah(chip, raw,
					*fcc_uah, batt_temp, chargecycles);
	pr_debug("RC = %uuAh\n", *remaining_charge_uah);

	/* calculate cc micro_volt_hour */
	calculate_cc_uah(chip, raw->cc, cc_uah);
	pr_debug("cc_uah = %duAh raw->cc = %x cc = %lld after subtracting %x\n",
				*cc_uah, raw->cc,
				(int64_t)raw->cc - chip->cc_reading_at_100,
				chip->cc_reading_at_100);

	soc_rbatt = ((*remaining_charge_uah - *cc_uah) * 100) / *fcc_uah;
	if (soc_rbatt < 0)
		soc_rbatt = 0;
	*rbatt = get_rbatt(chip, soc_rbatt, batt_temp);

	calculate_iavg_ua(chip, *cc_uah, iavg_ua, delta_time_us);

	*unusable_charge_uah = calculate_unusable_charge_uah(chip, *rbatt,
					*fcc_uah, *cc_uah, soc_rbatt,
					batt_temp, chargecycles, *iavg_ua);
	pr_debug("UUC = %uuAh\n", *unusable_charge_uah);
}

static int calculate_real_fcc_uah(struct pm8921_bms_chip *chip,
				struct pm8921_soc_params *raw,
				int batt_temp, int chargecycles,
				int *ret_fcc_uah)
{
	int fcc_uah, unusable_charge_uah;
	int remaining_charge_uah;
	int cc_uah;
	int real_fcc_uah;
	int rbatt;
	int iavg_ua;
	int delta_time_us;

	calculate_soc_params(chip, raw, batt_temp, chargecycles,
						&fcc_uah,
						&unusable_charge_uah,
						&remaining_charge_uah,
						&cc_uah,
						&rbatt,
						&iavg_ua,
						&delta_time_us);

	real_fcc_uah = remaining_charge_uah - cc_uah;
	*ret_fcc_uah = fcc_uah;
	pr_debug("real_fcc = %d, RC = %d CC = %d fcc = %d\n",
			real_fcc_uah, remaining_charge_uah, cc_uah, fcc_uah);
	return real_fcc_uah;
}

static int bound_soc(int soc)
{
	soc = max(0, soc);
	soc = min(100, soc);
	return soc;
}

static int last_soc_est = -EINVAL;
static int adjust_soc(struct pm8921_bms_chip *chip, int soc, int batt_temp,
		int rbatt , int fcc_uah, int uuc_uah, int cc_uah)
{
	int ibat_ua = 0, vbat_uv = 0;
	int ocv_est_uv = 0, soc_est = 0, pc_est = 0, pc = 0;
	int delta_ocv_uv = 0;
	int n = 0;
	int rc_new_uah = 0;
	int pc_new = 0;
	int soc_new = 0;
	int m = 0;

	pm8921_bms_get_simultaneous_battery_voltage_and_current(&ibat_ua,
		&vbat_uv);

	if (ibat_ua < 0)
		goto out;
	ocv_est_uv = vbat_uv + (ibat_ua * rbatt)/1000;
	pc_est = calculate_pc(chip, ocv_est_uv, batt_temp, last_chargecycles);
	soc_est = div_s64((s64)fcc_uah * pc_est - uuc_uah*100,
						(s64)fcc_uah - uuc_uah);
	soc_est = bound_soc(soc_est);

	/*
	 * do not adjust if soc_est is between 45 and 25 OR soc_est is
	 * same as what bms calculated
	 */
	if (is_between(45, 25, soc_est) || soc_est == soc)
		goto out;

	if (last_soc_est == -EINVAL)
		last_soc_est = soc;

	n = min(200, max(1 , soc + soc_est + last_soc_est));
	/* remember the last soc_est in last_soc_est */
	last_soc_est = soc_est;

	pc = calculate_pc(chip, chip->last_ocv_uv,
				batt_temp, last_chargecycles);
	if (pc > 0) {
		pc_new = calculate_pc(chip, chip->last_ocv_uv - (++m * 1000),
						batt_temp, last_chargecycles);
		while (pc_new == pc) {
			/* start taking 10mV steps */
			m = m + 10;
			pc_new = calculate_pc(chip,
						chip->last_ocv_uv - (m * 1000),
						batt_temp, last_chargecycles);
		}
	} else {
		/*
		 * pc is already at the lowest point,
		 * assume 1 millivolt translates to 1% pc
		 */
		pc = 1;
		pc_new = 0;
		m = 1;
	}

	delta_ocv_uv = div_s64((soc - soc_est) * (s64)m * 1000,
							n * (pc - pc_new));
	chip->last_ocv_uv -= delta_ocv_uv;

	if (chip->last_ocv_uv >= chip->max_voltage_uv)
		chip->last_ocv_uv = chip->max_voltage_uv;

	/* calculate the soc based on this new ocv */
	pc_new = calculate_pc(chip, chip->last_ocv_uv,
						batt_temp, last_chargecycles);
	rc_new_uah = (fcc_uah * pc_new) / 100;
	soc_new = (rc_new_uah - cc_uah - uuc_uah)*100 / (fcc_uah - uuc_uah);
	soc_new = bound_soc(soc_new);

	/*
	 * if soc_new is ZERO force it higher so that phone doesnt report soc=0
	 * soc = 0 should happen only when soc_est == 0
	 */
	if (soc_new == 0 && soc_est != 0)
		soc_new = 1;

	soc = soc_new;

out:
	pr_debug("ibat_ua = %d, vbat_uv = %d, ocv_est_uv = %d, pc_est = %d, "
		"soc_est = %d, n = %d, delta_ocv_uv = %d, last_ocv_uv = %d, "
		"pc_new = %d, soc_new = %d\n",
		ibat_ua, vbat_uv, ocv_est_uv, pc_est,
		soc_est, n, delta_ocv_uv, chip->last_ocv_uv,
		pc_new, soc_new);

	return soc;
}

#define MAX_SHUTDOWN_ADJUST_SECONDS	1800
static int adjust_for_shutdown_soc(struct pm8921_bms_chip *chip, int soc)
{
	struct timespec uptime;
	int val;

	/* value of zero means the shutdown soc should not be used */
	if (chip->shutdown_soc == 0)
		return soc;

	if (shutdown_soc_invalid) {
		chip->shutdown_soc = 0;
		return soc;
	}

	do_posix_clock_monotonic_gettime(&uptime);

	if (uptime.tv_sec >= MAX_SHUTDOWN_ADJUST_SECONDS) {
		/*
		 * adjusted for a long time now, switch to reporting the
		 * calculated soc
		 */
		chip->shutdown_soc = 0;
		return soc;
	}

	val = ((MAX_SHUTDOWN_ADJUST_SECONDS - uptime.tv_sec)
		* chip->shutdown_soc
		+ uptime.tv_sec * soc);
	val /= MAX_SHUTDOWN_ADJUST_SECONDS;
	pr_debug("shutdown_soc = %d, adj soc = %d, calc soc = %d\n",
				chip->shutdown_soc, val, soc);

	return val;
}

static void backup_soc(struct pm8921_bms_chip *chip, int last_soc)
{
	/* TODO: if 0x107 is free for all variants 8917, 8038 etc */
	pm8xxx_writeb(the_chip->dev->parent, TEMP_SOC_STORAGE, last_soc);
}

static void read_shutdown_soc(struct pm8921_bms_chip *chip)
{
	int rc;
	u8 temp;

	rc = pm8xxx_readb(chip->dev->parent, TEMP_SOC_STORAGE, &temp);
	if (rc)
		pr_err("failed to read addr = %d %d\n", TEMP_SOC_STORAGE, rc);
	else
		chip->shutdown_soc = temp;

	pr_debug("shutdown_soc = %d\n", chip->shutdown_soc);
}

void pm8921_bms_invalidate_shutdown_soc(void)
{
	pr_debug("Invalidating shutdown soc - the battery was removed\n");
	shutdown_soc_invalid = 1;
	if (the_chip)
		the_chip->shutdown_soc = 0;
}
EXPORT_SYMBOL(pm8921_bms_invalidate_shutdown_soc);

/*
 * Remaining Usable Charge = remaining_charge (charge at ocv instance)
 *				- coloumb counter charge
 *				- unusable charge (due to battery resistance)
 * SOC% = (remaining usable charge/ fcc - usable_charge);
 */
static int calculate_state_of_charge(struct pm8921_bms_chip *chip,
					struct pm8921_soc_params *raw,
					int batt_temp, int chargecycles)
{
	int remaining_usable_charge_uah, fcc_uah, unusable_charge_uah;
	int remaining_charge_uah, soc;
	int cc_uah;
	int rbatt;
	int shutdown_adjusted_soc;
	int iavg_ua;
	int delta_time_us;

	calculate_soc_params(chip, raw, batt_temp, chargecycles,
						&fcc_uah,
						&unusable_charge_uah,
						&remaining_charge_uah,
						&cc_uah,
						&rbatt,
						&iavg_ua,
						&delta_time_us);

	/* calculate remaining usable charge */
	remaining_usable_charge_uah = remaining_charge_uah
					- cc_uah
					- unusable_charge_uah;

	pr_debug("RUC = %duAh\n", remaining_usable_charge_uah);
	if (fcc_uah - unusable_charge_uah <= 0) {
		pr_warn("FCC = %duAh, UUC = %duAh forcing soc = 0\n",
						fcc_uah, unusable_charge_uah);
		soc = 0;
	} else {
		soc = (remaining_usable_charge_uah * 100)
			/ (fcc_uah - unusable_charge_uah);
	}

	read_soc_original_param = soc;
	soc = (remaining_usable_charge_uah * 105)
		/ (fcc_uah - unusable_charge_uah);
	if (read_soc_expand_param == -EINVAL)
		read_soc_expand_param = soc;
	else if (soc > read_soc_expand_param)
		soc = read_soc_expand_param++;
	else if (soc < read_soc_expand_param)
		soc = read_soc_expand_param--;

	if (soc > 100)
		soc = 100;
	pr_debug("SOC = %u%%\n", soc);

	if (bms_fake_battery != -EINVAL) {
		pr_debug("Returning Fake SOC = %d%%\n", bms_fake_battery);
		return bms_fake_battery;
	}

	if (soc < 0) {
		pr_err("bad rem_usb_chg = %d rem_chg %d,"
				"cc_uah %d, unusb_chg %d\n",
				remaining_usable_charge_uah,
				remaining_charge_uah,
				cc_uah, unusable_charge_uah);

		pr_err("for bad rem_usb_chg last_ocv_uv = %d"
				"chargecycles = %d, batt_temp = %d"
				"fcc = %d soc =%d\n",
				chip->last_ocv_uv, chargecycles, batt_temp,
				fcc_uah, soc);
		soc = 0;
	}

	soc = adjust_soc(chip, soc, batt_temp, rbatt,
					fcc_uah, unusable_charge_uah, cc_uah);

	if (last_soc == -EINVAL || soc <= last_soc) {
		last_soc = soc;
	} else {
		/*
		 * soc > last_soc
		 * the device must be charging for reporting a higher soc, if
		 * not ignore this soc and continue reporting the last_soc
		 */
		if (the_chip->start_percent != -EINVAL)
			last_soc = soc;
		else
			pr_debug("soc = %d reporting last_soc = %d\n", soc,
								last_soc);
	}

	shutdown_adjusted_soc = adjust_for_shutdown_soc(chip, last_soc);
	backup_soc(chip, shutdown_adjusted_soc);

	return shutdown_adjusted_soc;
}

#define MIN_DELTA_625_UV	1000
static void calib_hkadc(struct pm8921_bms_chip *chip)
{
	int voltage, rc;
	struct pm8xxx_adc_chan_result result;
	int usb_chg;
	int this_delta;

	rc = pm8xxx_adc_read(the_chip->ref1p25v_channel, &result);
	if (rc) {
		pr_err("ADC failed for 1.25volts rc = %d\n", rc);
		return;
	}
	voltage = xoadc_reading_to_microvolt(result.adc_code);

	pr_debug("result 1.25v = 0x%x, voltage = %duV adc_meas = %lld\n",
				result.adc_code, voltage, result.measurement);

	chip->xoadc_v125 = voltage;

	rc = pm8xxx_adc_read(the_chip->ref625mv_channel, &result);
	if (rc) {
		pr_err("ADC failed for 1.25volts rc = %d\n", rc);
		return;
	}
	voltage = xoadc_reading_to_microvolt(result.adc_code);

	usb_chg = usb_chg_plugged_in();
	pr_debug("result 0.625V = 0x%x, voltage = %duV adc_meas = %lld "
				"usb_chg = %d\n",
				result.adc_code, voltage, result.measurement,
				usb_chg);

	if (usb_chg)
		chip->xoadc_v0625_usb_present = voltage;
	else
		chip->xoadc_v0625_usb_absent = voltage;

	chip->xoadc_v0625 = voltage;
	if (chip->xoadc_v0625_usb_present && chip->xoadc_v0625_usb_absent) {
		this_delta = chip->xoadc_v0625_usb_present
						- chip->xoadc_v0625_usb_absent;
		pr_debug("this_delta= %duV\n", this_delta);
		if (this_delta > MIN_DELTA_625_UV)
			last_usb_cal_delta_uv = this_delta;
		pr_debug("625V_present= %d, 625V_absent= %d, delta = %duV\n",
			chip->xoadc_v0625_usb_present,
			chip->xoadc_v0625_usb_absent,
			last_usb_cal_delta_uv);
	}
}

static void calibrate_hkadc_work(struct work_struct *work)
{
	struct pm8921_bms_chip *chip = container_of(work,
				struct pm8921_bms_chip, calib_hkadc_work);

	calib_hkadc(chip);
}

void pm8921_bms_calibrate_hkadc(void)
{
	schedule_work(&the_chip->calib_hkadc_work);
}

int pm8921_bms_get_vsense_avg(int *result)
{
	int rc = -EINVAL;

	if (the_chip) {
		mutex_lock(&the_chip->bms_output_lock);
		pm_bms_lock_output_data(the_chip);
		rc = read_vsense_avg(the_chip, result);
		pm_bms_unlock_output_data(the_chip);
		mutex_unlock(&the_chip->bms_output_lock);
	}

	pr_err("called before initialization\n");
	return rc;
}
EXPORT_SYMBOL(pm8921_bms_get_vsense_avg);

int pm8921_bms_get_battery_current(int *result_ua)
{
	int vsense;

	if (!the_chip) {
		pr_err("called before initialization\n");
		return -EINVAL;
	}
	if (the_chip->r_sense == 0) {
		pr_err("r_sense is zero\n");
		return -EINVAL;
	}

	mutex_lock(&the_chip->bms_output_lock);
	pm_bms_lock_output_data(the_chip);
	read_vsense_avg(the_chip, &vsense);
	pm_bms_unlock_output_data(the_chip);
	mutex_unlock(&the_chip->bms_output_lock);
	pr_debug("vsense=%duV\n", vsense);
	/* cast for signed division */
	*result_ua = vsense * 1000 / (int)the_chip->r_sense;
	pr_debug("ibat=%duA\n", *result_ua);
	return 0;
}
EXPORT_SYMBOL(pm8921_bms_get_battery_current);

int pm8921_bms_get_percent_charge(void)
{
	int batt_temp, rc;
	struct pm8xxx_adc_chan_result result;
	struct pm8921_soc_params raw;
	int soc;

	if (!the_chip) {
		pr_err("called before initialization\n");
		return -EINVAL;
	}

	rc = pm8xxx_adc_read(the_chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					the_chip->batt_temp_channel, rc);
		return rc;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx", result.physical,
						result.measurement);
	batt_temp = (int)result.physical;

	mutex_lock(&the_chip->last_ocv_uv_mutex);
	read_soc_params_raw(the_chip, &raw);

	soc = calculate_state_of_charge(the_chip, &raw,
					batt_temp, last_chargecycles);
	mutex_unlock(&the_chip->last_ocv_uv_mutex);
	return soc;
}
EXPORT_SYMBOL_GPL(pm8921_bms_get_percent_charge);

int pm8921_bms_get_rbatt(void)
{
	int batt_temp, rc;
	struct pm8xxx_adc_chan_result result;
	struct pm8921_soc_params raw;
	int fcc_uah;
	int unusable_charge_uah;
	int remaining_charge_uah;
	int cc_uah;
	int rbatt;
	int iavg_ua;
	int delta_time_us;

	if (!the_chip) {
		pr_err("called before initialization\n");
		return -EINVAL;
	}

	rc = pm8xxx_adc_read(the_chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					the_chip->batt_temp_channel, rc);
		return rc;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	batt_temp = (int)result.physical;

	mutex_lock(&the_chip->last_ocv_uv_mutex);

	read_soc_params_raw(the_chip, &raw);

	calculate_soc_params(the_chip, &raw, batt_temp, last_chargecycles,
						&fcc_uah,
						&unusable_charge_uah,
						&remaining_charge_uah,
						&cc_uah,
						&rbatt,
						&iavg_ua,
						&delta_time_us);
	mutex_unlock(&the_chip->last_ocv_uv_mutex);

	return rbatt;
}
EXPORT_SYMBOL_GPL(pm8921_bms_get_rbatt);

int pm8921_bms_get_init_fcc(void)
{
	if (!the_chip) {
		pr_err("called before initialization\n");
		return -EINVAL;
	}
	return (int)the_chip->fcc;
}
EXPORT_SYMBOL_GPL(pm8921_bms_get_init_fcc);

int pm8921_bms_get_fcc(void)
{
	int batt_temp, rc;
	struct pm8xxx_adc_chan_result result;

	if (!the_chip) {
		pr_err("called before initialization\n");
		return -EINVAL;
	}

	rc = pm8xxx_adc_read(the_chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					the_chip->batt_temp_channel, rc);
		return rc;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx", result.physical,
						result.measurement);
	batt_temp = (int)result.physical;
	return calculate_fcc_uah(the_chip, batt_temp, last_chargecycles);
}
EXPORT_SYMBOL_GPL(pm8921_bms_get_fcc);

#define IBAT_TOL_MASK		0x0F
#define OCV_TOL_MASK			0xF0
#define IBAT_TOL_DEFAULT	0x03
#define IBAT_TOL_NOCHG		0x0F
#define OCV_TOL_DEFAULT		0x20
#define OCV_TOL_NO_OCV		0x00
void pm8921_bms_charging_began(void)
{
	int batt_temp, rc;
	struct pm8xxx_adc_chan_result result;
	struct pm8921_soc_params raw;

	rc = pm8xxx_adc_read(the_chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
				the_chip->batt_temp_channel, rc);
		return;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	batt_temp = (int)result.physical;

	mutex_lock(&the_chip->last_ocv_uv_mutex);
	read_soc_params_raw(the_chip, &raw);

	the_chip->start_percent = calculate_state_of_charge(the_chip, &raw,
					batt_temp, last_chargecycles);
	mutex_unlock(&the_chip->last_ocv_uv_mutex);

	bms_start_percent = the_chip->start_percent;
	bms_start_ocv_uv = raw.last_good_ocv_uv;
	calculate_cc_uah(the_chip, raw.cc, &bms_start_cc_uah);
	pm_bms_masked_write(the_chip, BMS_TOLERANCES,
			IBAT_TOL_MASK, IBAT_TOL_DEFAULT);
	pr_debug("start_percent = %u%%\n", the_chip->start_percent);
}
EXPORT_SYMBOL_GPL(pm8921_bms_charging_began);

#define DELTA_FCC_PERCENT	100
#define MIN_START_PERCENT_FOR_LEARNING	30
void pm8921_bms_charging_end(int is_battery_full)
{
	int batt_temp, rc;
	struct pm8xxx_adc_chan_result result;
	struct pm8921_soc_params raw;

	if (the_chip == NULL)
		return;

	rc = pm8xxx_adc_read(the_chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
				the_chip->batt_temp_channel, rc);
		return;
	}
	pr_debug("batt_temp phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	batt_temp = (int)result.physical;

	mutex_lock(&the_chip->last_ocv_uv_mutex);

	read_soc_params_raw(the_chip, &raw);

	calculate_cc_uah(the_chip, raw.cc, &bms_end_cc_uah);

	bms_end_ocv_uv = raw.last_good_ocv_uv;

	if (is_battery_full && the_chip->enable_fcc_learning
		&& the_chip->start_percent <= MIN_START_PERCENT_FOR_LEARNING) {
		int fcc_uah, new_fcc_uah, delta_fcc_uah;

		new_fcc_uah = calculate_real_fcc_uah(the_chip, &raw,
						batt_temp, last_chargecycles,
						&fcc_uah);
		delta_fcc_uah = new_fcc_uah - fcc_uah;
		if (delta_fcc_uah < 0)
			delta_fcc_uah = -delta_fcc_uah;

		if (delta_fcc_uah * 100  > (DELTA_FCC_PERCENT * fcc_uah)) {
			/* new_fcc_uah is outside the scope limit it */
			if (new_fcc_uah > fcc_uah)
				new_fcc_uah
				= (fcc_uah +
					(DELTA_FCC_PERCENT * fcc_uah) / 100);
			else
				new_fcc_uah
				= (fcc_uah -
					(DELTA_FCC_PERCENT * fcc_uah) / 100);

			pr_debug("delta_fcc=%d > %d percent of fcc=%d"
					"restring it to %d\n",
					delta_fcc_uah, DELTA_FCC_PERCENT,
					fcc_uah, new_fcc_uah);
			read_soc_expand_param = -EINVAL;
		}

		last_real_fcc_mah = new_fcc_uah/1000;
		last_real_fcc_batt_temp = batt_temp;
		readjust_fcc_table();

	}

	if (is_battery_full) {
		the_chip->ocv_reading_at_100 = raw.last_good_ocv_raw;
		the_chip->cc_reading_at_100 = raw.cc;

		the_chip->last_ocv_uv = the_chip->max_voltage_uv;
		raw.last_good_ocv_uv = the_chip->max_voltage_uv;
		/*
		 * since we are treating this as an ocv event
		 * forget the old cc value
		 */
		the_chip->last_cc_uah = 0;
		pr_debug("EOC ocv_reading = 0x%x cc = 0x%x\n",
				the_chip->ocv_reading_at_100,
				the_chip->cc_reading_at_100);
	}

	the_chip->end_percent = calculate_state_of_charge(the_chip, &raw,
					batt_temp, last_chargecycles);
	mutex_unlock(&the_chip->last_ocv_uv_mutex);

	bms_end_percent = the_chip->end_percent;

	if (the_chip->end_percent > the_chip->start_percent) {
		last_charge_increase +=
			the_chip->end_percent - the_chip->start_percent;
		if (last_charge_increase > 100) {
			last_chargecycles++;
			last_charge_increase = last_charge_increase % 100;
		}
	}
	pr_debug("end_percent = %u%% last_charge_increase = %d"
			"last_chargecycles = %d\n",
			the_chip->end_percent,
			last_charge_increase,
			last_chargecycles);
	the_chip->start_percent = -EINVAL;
	the_chip->end_percent = -EINVAL;
	pm_bms_masked_write(the_chip, BMS_TOLERANCES,
				IBAT_TOL_MASK, IBAT_TOL_NOCHG);
}
EXPORT_SYMBOL_GPL(pm8921_bms_charging_end);

int pm8921_bms_stop_ocv_updates(struct pm8921_bms_chip *chip)
{
	pr_debug("stopping ocv updates\n");
	return pm_bms_masked_write(chip, BMS_TOLERANCES,
			OCV_TOL_MASK, OCV_TOL_NO_OCV);
}
EXPORT_SYMBOL_GPL(pm8921_bms_stop_ocv_updates);

int pm8921_bms_start_ocv_updates(struct pm8921_bms_chip *chip)
{
	pr_debug("stopping ocv updates\n");
	return pm_bms_masked_write(chip, BMS_TOLERANCES,
			OCV_TOL_MASK, OCV_TOL_DEFAULT);
}
EXPORT_SYMBOL_GPL(pm8921_bms_start_ocv_updates);

static irqreturn_t pm8921_bms_sbi_write_ok_handler(int irq, void *data)
{
	pr_debug("irq = %d triggered", irq);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_cc_thr_handler(int irq, void *data)
{
	pr_debug("irq = %d triggered", irq);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_vsense_thr_handler(int irq, void *data)
{
	pr_debug("irq = %d triggered", irq);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_vsense_for_r_handler(int irq, void *data)
{
	pr_debug("irq = %d triggered", irq);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_ocv_for_r_handler(int irq, void *data)
{
	struct pm8921_bms_chip *chip = data;

	pr_debug("irq = %d triggered", irq);
	schedule_work(&chip->calib_hkadc_work);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_good_ocv_handler(int irq, void *data)
{
	struct pm8921_bms_chip *chip = data;

	pr_debug("irq = %d triggered", irq);
	schedule_work(&chip->calib_hkadc_work);
	return IRQ_HANDLED;
}

static irqreturn_t pm8921_bms_vsense_avg_handler(int irq, void *data)
{
	pr_debug("irq = %d triggered", irq);
	return IRQ_HANDLED;
}

struct pm_bms_irq_init_data {
	unsigned int	irq_id;
	char		*name;
	unsigned long	flags;
	irqreturn_t	(*handler)(int, void *);
};

#define BMS_IRQ(_id, _flags, _handler) \
{ \
	.irq_id		= _id, \
	.name		= #_id, \
	.flags		= _flags, \
	.handler	= _handler, \
}

struct pm_bms_irq_init_data bms_irq_data[] = {
	BMS_IRQ(PM8921_BMS_SBI_WRITE_OK, IRQF_TRIGGER_RISING,
				pm8921_bms_sbi_write_ok_handler),
	BMS_IRQ(PM8921_BMS_CC_THR, IRQF_TRIGGER_RISING,
				pm8921_bms_cc_thr_handler),
	BMS_IRQ(PM8921_BMS_VSENSE_THR, IRQF_TRIGGER_RISING,
				pm8921_bms_vsense_thr_handler),
	BMS_IRQ(PM8921_BMS_VSENSE_FOR_R, IRQF_TRIGGER_RISING,
				pm8921_bms_vsense_for_r_handler),
	BMS_IRQ(PM8921_BMS_OCV_FOR_R, IRQF_TRIGGER_RISING,
				pm8921_bms_ocv_for_r_handler),
	BMS_IRQ(PM8921_BMS_GOOD_OCV, IRQF_TRIGGER_RISING,
				pm8921_bms_good_ocv_handler),
	BMS_IRQ(PM8921_BMS_VSENSE_AVG, IRQF_TRIGGER_RISING,
				pm8921_bms_vsense_avg_handler),
};

static void free_irqs(struct pm8921_bms_chip *chip)
{
	int i;

	for (i = 0; i < PM_BMS_MAX_INTS; i++)
		if (chip->pmic_bms_irq[i]) {
			free_irq(chip->pmic_bms_irq[i], NULL);
			chip->pmic_bms_irq[i] = 0;
		}
}

static int __devinit request_irqs(struct pm8921_bms_chip *chip,
					struct platform_device *pdev)
{
	struct resource *res;
	int ret, i;

	ret = 0;
	bitmap_fill(chip->enabled_irqs, PM_BMS_MAX_INTS);

	for (i = 0; i < ARRAY_SIZE(bms_irq_data); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
				bms_irq_data[i].name);
		if (res == NULL) {
			pr_err("couldn't find %s\n", bms_irq_data[i].name);
			goto err_out;
		}
		ret = request_irq(res->start, bms_irq_data[i].handler,
			bms_irq_data[i].flags,
			bms_irq_data[i].name, chip);
		if (ret < 0) {
			pr_err("couldn't request %d (%s) %d\n", res->start,
					bms_irq_data[i].name, ret);
			goto err_out;
		}
		chip->pmic_bms_irq[bms_irq_data[i].irq_id] = res->start;
		pm8921_bms_disable_irq(chip, bms_irq_data[i].irq_id);
	}
	return 0;

err_out:
	free_irqs(chip);
	return -EINVAL;
}

static int pm8921_bms_suspend(struct device *dev)
{
	int rc;
	struct pm8xxx_adc_chan_result result;
	struct pm8921_bms_chip *chip = dev_get_drvdata(dev);
	struct pm8921_soc_params raw;
	int fcc_uah;
	int remaining_charge_uah;
	int cc_uah;

	chip->batt_temp_suspend = 0;
	rc = pm8xxx_adc_read(chip->batt_temp_channel, &result);
	if (rc) {
		pr_err("error reading adc channel = %d, rc = %d\n",
					chip->batt_temp_channel, rc);
	}
	chip->batt_temp_suspend = (int)result.physical;

	mutex_lock(&chip->last_ocv_uv_mutex);
	read_soc_params_raw(chip, &raw);

	fcc_uah = calculate_fcc_uah(chip,
			chip->batt_temp_suspend, last_chargecycles);
	pr_debug("FCC = %uuAh batt_temp = %d, cycles = %d\n",
			fcc_uah, chip->batt_temp_suspend, last_chargecycles);
	/* calculate remainging charge */
	remaining_charge_uah = calculate_remaining_charge_uah(chip, &raw,
					fcc_uah, chip->batt_temp_suspend,
					last_chargecycles);
	pr_debug("RC = %uuAh\n", remaining_charge_uah);

	/* calculate cc micro_volt_hour */
	calculate_cc_uah(chip, raw.cc, &cc_uah);
	pr_debug("cc_uah = %duAh raw->cc = %x cc = %lld after subtracting %x\n",
				cc_uah, raw.cc,
				(int64_t)raw.cc - chip->cc_reading_at_100,
				chip->cc_reading_at_100);
	chip->soc_rbatt_suspend = ((remaining_charge_uah - cc_uah) * 100)
						/ fcc_uah;
	mutex_unlock(&chip->last_ocv_uv_mutex);

	return 0;
}

#define DELTA_RBATT_PERCENT	10
static int pm8921_bms_resume(struct device *dev)
{
	struct pm8921_rbatt_params raw;
	struct pm8921_bms_chip *chip = dev_get_drvdata(dev);
	int rbatt;
	int expected_rbatt;
	int scalefactor;
	int delta_rbatt;

	read_rbatt_params_raw(chip, &raw);
	rbatt = calculate_rbatt_resume(chip, &raw);

	if (rbatt < 0)
		return 0;

	expected_rbatt
		= (last_rbatt < 0) ? chip->default_rbatt_mohm : last_rbatt;

	if (chip->rbatt_sf_lut) {
		scalefactor = interpolate_scalingfactor(chip,
						chip->rbatt_sf_lut,
						chip->batt_temp_suspend / 10,
						chip->soc_rbatt_suspend);
		rbatt = rbatt * 100 / scalefactor;
	}

	delta_rbatt = expected_rbatt - rbatt;
	if (delta_rbatt)
		delta_rbatt = -delta_rbatt;
	/*
	 * only update last_rbatt if rbatt is within some
	 * percent of expected_rbatt
	 */
	if (delta_rbatt * 100 <= DELTA_RBATT_PERCENT * expected_rbatt)
		last_rbatt = rbatt;

	return 0;
}

static const struct dev_pm_ops pm8921_pm_ops = {
	.suspend	= pm8921_bms_suspend,
	.resume		= pm8921_bms_resume,
};
#define EN_BMS_BIT	BIT(7)
#define EN_PON_HS_BIT	BIT(0)
static int __devinit pm8921_bms_hw_init(struct pm8921_bms_chip *chip)
{
	int rc;

	rc = pm_bms_masked_write(chip, BMS_CONTROL,
			EN_BMS_BIT | EN_PON_HS_BIT, EN_BMS_BIT | EN_PON_HS_BIT);
	if (rc) {
		pr_err("failed to enable pon and bms addr = %d %d",
				BMS_CONTROL, rc);
	}

	/* The charger will call start charge later if usb is present */
	pm_bms_masked_write(chip, BMS_TOLERANCES,
				IBAT_TOL_MASK, IBAT_TOL_NOCHG);
	return 0;
}

static void check_initial_ocv(struct pm8921_bms_chip *chip)
{
	int ocv_uv, rc;
	int16_t ocv_raw;
	int usb_chg;

	/*
	 * Check if a ocv is available in bms hw,
	 * if not compute it here at boot time and save it
	 * in the last_ocv_uv.
	 */
	ocv_uv = 0;
	pm_bms_read_output_data(chip, LAST_GOOD_OCV_VALUE, &ocv_raw);
	usb_chg = usb_chg_plugged_in();
	rc = convert_vbatt_raw_to_uv(chip, usb_chg, ocv_raw, &ocv_uv);
	if (rc || ocv_uv == 0) {
		rc = adc_based_ocv(chip, &ocv_uv);
		if (rc) {
			pr_err("failed to read adc based ocv_uv rc = %d\n", rc);
			ocv_uv = DEFAULT_OCV_MICROVOLTS;
		}
	}
	chip->last_ocv_uv = ocv_uv;
	pr_debug("ocv_uv = %d last_ocv_uv = %d\n", ocv_uv, chip->last_ocv_uv);
}

static int64_t read_battery_id(struct pm8921_bms_chip *chip)
{
	int rc;
	struct pm8xxx_adc_chan_result result;

	rc = pm8xxx_adc_read(chip->batt_id_channel, &result);
	if (rc) {
		pr_err("error reading batt id channel = %d, rc = %d\n",
					chip->vbat_channel, rc);
		return rc;
	}
	pr_debug("batt_id phy = %lld meas = 0x%llx\n", result.physical,
						result.measurement);
	return result.adc_code;
}

#define PALLADIUM_ID_MIN	0x7F40
#define PALLADIUM_ID_MAX	0x7F5A
#define DESAY_5200_ID_MIN	0x7F7F
#define DESAY_5200_ID_MAX	0x802F
static int set_battery_data(struct pm8921_bms_chip *chip)
{
	int64_t battery_id;

	if (chip->batt_type == BATT_DESAY)
		goto desay;
	else if (chip->batt_type == BATT_PALLADIUM)
		goto palladium;

	battery_id = read_battery_id(chip);
	if (battery_id < 0) {
		pr_err("cannot read battery id err = %lld\n", battery_id);
		return battery_id;
	}

	if (is_between(PALLADIUM_ID_MIN, PALLADIUM_ID_MAX, battery_id)) {
		goto palladium;
	} else if (is_between(DESAY_5200_ID_MIN, DESAY_5200_ID_MAX,
				battery_id)) {
		goto desay;
	} else {
		pr_warn("invalid battid, palladium 1500 assumed batt_id %llx\n",
				battery_id);
		goto palladium;
	}

palladium:
		chip->fcc = palladium_1500_data.fcc;
		chip->fcc_temp_lut = palladium_1500_data.fcc_temp_lut;
		chip->fcc_sf_lut = palladium_1500_data.fcc_sf_lut;
		chip->pc_temp_ocv_lut = palladium_1500_data.pc_temp_ocv_lut;
		chip->pc_sf_lut = palladium_1500_data.pc_sf_lut;
		chip->rbatt_sf_lut = palladium_1500_data.rbatt_sf_lut;
		chip->default_rbatt_mohm
				= palladium_1500_data.default_rbatt_mohm;
		chip->delta_rbatt_mohm = palladium_1500_data.delta_rbatt_mohm;
		return 0;
desay:
		chip->fcc = desay_5200_data.fcc;
		chip->fcc_temp_lut = desay_5200_data.fcc_temp_lut;
		chip->pc_temp_ocv_lut = desay_5200_data.pc_temp_ocv_lut;
		chip->pc_sf_lut = desay_5200_data.pc_sf_lut;
		chip->rbatt_sf_lut = desay_5200_data.rbatt_sf_lut;
		chip->default_rbatt_mohm = desay_5200_data.default_rbatt_mohm;
		chip->delta_rbatt_mohm = desay_5200_data.delta_rbatt_mohm;
		return 0;
}

enum bms_request_operation {
	CALC_RBATT,
	CALC_FCC,
	CALC_PC,
	CALC_SOC,
	CALIB_HKADC,
	CALIB_CCADC,
	GET_VBAT_VSENSE_SIMULTANEOUS,
	STOP_OCV,
	START_OCV,
};

static int test_batt_temp = 5;
static int test_chargecycle = 150;
static int test_ocv = 3900000;
enum {
	TEST_BATT_TEMP,
	TEST_CHARGE_CYCLE,
	TEST_OCV,
};
static int get_test_param(void *data, u64 * val)
{
	switch ((int)data) {
	case TEST_BATT_TEMP:
		*val = test_batt_temp;
		break;
	case TEST_CHARGE_CYCLE:
		*val = test_chargecycle;
		break;
	case TEST_OCV:
		*val = test_ocv;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
static int set_test_param(void *data, u64  val)
{
	switch ((int)data) {
	case TEST_BATT_TEMP:
		test_batt_temp = (int)val;
		break;
	case TEST_CHARGE_CYCLE:
		test_chargecycle = (int)val;
		break;
	case TEST_OCV:
		test_ocv = (int)val;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(temp_fops, get_test_param, set_test_param, "%llu\n");

static int get_calc(void *data, u64 * val)
{
	int param = (int)data;
	int ret = 0;
	int ibat_ua, vbat_uv;
	struct pm8921_soc_params raw;
	struct pm8921_rbatt_params rraw;

	read_soc_params_raw(the_chip, &raw);
	read_rbatt_params_raw(the_chip, &rraw);

	*val = 0;

	/* global irq number passed in via data */
	switch (param) {
	case CALC_RBATT:
		*val = calculate_rbatt_resume(the_chip, &rraw);
		break;
	case CALC_FCC:
		*val = calculate_fcc_uah(the_chip, test_batt_temp,
							test_chargecycle);
		break;
	case CALC_PC:
		*val = calculate_pc(the_chip, test_ocv, test_batt_temp,
							test_chargecycle);
		break;
	case CALC_SOC:
		*val = calculate_state_of_charge(the_chip, &raw,
					test_batt_temp, test_chargecycle);
		break;
	case CALIB_HKADC:
		/* reading this will trigger calibration */
		*val = 0;
		calib_hkadc(the_chip);
		break;
	case CALIB_CCADC:
		/* reading this will trigger calibration */
		*val = 0;
		pm8xxx_calib_ccadc();
		break;
	case GET_VBAT_VSENSE_SIMULTANEOUS:
		/* reading this will call simultaneous vbat and vsense */
		*val =
		pm8921_bms_get_simultaneous_battery_voltage_and_current(
			&ibat_ua,
			&vbat_uv);
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int set_calc(void *data, u64 val)
{
	int param = (int)data;
	int ret = 0;

	switch (param) {
	case STOP_OCV:
		pm8921_bms_stop_ocv_updates(the_chip);
		break;
	case START_OCV:
		pm8921_bms_start_ocv_updates(the_chip);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(calc_fops, get_calc, set_calc, "%llu\n");

static int get_reading(void *data, u64 * val)
{
	int param = (int)data;
	int ret = 0;
	struct pm8921_soc_params raw;
	struct pm8921_rbatt_params rraw;

	read_soc_params_raw(the_chip, &raw);
	read_rbatt_params_raw(the_chip, &rraw);

	*val = 0;

	switch (param) {
	case CC_MSB:
	case CC_LSB:
		*val = raw.cc;
		break;
	case LAST_GOOD_OCV_VALUE:
		*val = raw.last_good_ocv_uv;
		break;
	case VBATT_FOR_RBATT:
		*val = rraw.vbatt_for_rbatt_uv;
		break;
	case VSENSE_FOR_RBATT:
		*val = rraw.vsense_for_rbatt_uv;
		break;
	case OCV_FOR_RBATT:
		*val = rraw.ocv_for_rbatt_uv;
		break;
	case VSENSE_AVG:
		read_vsense_avg(the_chip, (uint *)val);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(reading_fops, get_reading, NULL, "%lld\n");

static int get_rt_status(void *data, u64 * val)
{
	int i = (int)data;
	int ret;

	/* global irq number passed in via data */
	ret = pm_bms_get_rt_status(the_chip, i);
	*val = ret;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(rt_fops, get_rt_status, NULL, "%llu\n");

static int get_reg(void *data, u64 * val)
{
	int addr = (int)data;
	int ret;
	u8 temp;

	ret = pm8xxx_readb(the_chip->dev->parent, addr, &temp);
	if (ret) {
		pr_err("pm8xxx_readb to %x value = %d errored = %d\n",
			addr, temp, ret);
		return -EAGAIN;
	}
	*val = temp;
	return 0;
}

static int set_reg(void *data, u64 val)
{
	int addr = (int)data;
	int ret;
	u8 temp;

	temp = (u8) val;
	ret = pm8xxx_writeb(the_chip->dev->parent, addr, temp);
	if (ret) {
		pr_err("pm8xxx_writeb to %x value = %d errored = %d\n",
			addr, temp, ret);
		return -EAGAIN;
	}
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(reg_fops, get_reg, set_reg, "0x%02llx\n");

static void create_debugfs_entries(struct pm8921_bms_chip *chip)
{
	int i;

	chip->dent = debugfs_create_dir("pm8921-bms", NULL);

	if (IS_ERR(chip->dent)) {
		pr_err("pmic bms couldnt create debugfs dir\n");
		return;
	}

	debugfs_create_file("BMS_CONTROL", 0644, chip->dent,
			(void *)BMS_CONTROL, &reg_fops);
	debugfs_create_file("BMS_OUTPUT0", 0644, chip->dent,
			(void *)BMS_OUTPUT0, &reg_fops);
	debugfs_create_file("BMS_OUTPUT1", 0644, chip->dent,
			(void *)BMS_OUTPUT1, &reg_fops);
	debugfs_create_file("BMS_TEST1", 0644, chip->dent,
			(void *)BMS_TEST1, &reg_fops);

	debugfs_create_file("test_batt_temp", 0644, chip->dent,
				(void *)TEST_BATT_TEMP, &temp_fops);
	debugfs_create_file("test_chargecycle", 0644, chip->dent,
				(void *)TEST_CHARGE_CYCLE, &temp_fops);
	debugfs_create_file("test_ocv", 0644, chip->dent,
				(void *)TEST_OCV, &temp_fops);

	debugfs_create_file("read_cc", 0644, chip->dent,
				(void *)CC_MSB, &reading_fops);
	debugfs_create_file("read_last_good_ocv", 0644, chip->dent,
				(void *)LAST_GOOD_OCV_VALUE, &reading_fops);
	debugfs_create_file("read_vbatt_for_rbatt", 0644, chip->dent,
				(void *)VBATT_FOR_RBATT, &reading_fops);
	debugfs_create_file("read_vsense_for_rbatt", 0644, chip->dent,
				(void *)VSENSE_FOR_RBATT, &reading_fops);
	debugfs_create_file("read_ocv_for_rbatt", 0644, chip->dent,
				(void *)OCV_FOR_RBATT, &reading_fops);
	debugfs_create_file("read_vsense_avg", 0644, chip->dent,
				(void *)VSENSE_AVG, &reading_fops);

	debugfs_create_file("show_rbatt", 0644, chip->dent,
				(void *)CALC_RBATT, &calc_fops);
	debugfs_create_file("show_fcc", 0644, chip->dent,
				(void *)CALC_FCC, &calc_fops);
	debugfs_create_file("show_pc", 0644, chip->dent,
				(void *)CALC_PC, &calc_fops);
	debugfs_create_file("show_soc", 0644, chip->dent,
				(void *)CALC_SOC, &calc_fops);
	debugfs_create_file("calib_hkadc", 0644, chip->dent,
				(void *)CALIB_HKADC, &calc_fops);
	debugfs_create_file("calib_ccadc", 0644, chip->dent,
				(void *)CALIB_CCADC, &calc_fops);
	debugfs_create_file("stop_ocv", 0644, chip->dent,
				(void *)STOP_OCV, &calc_fops);
	debugfs_create_file("start_ocv", 0644, chip->dent,
				(void *)START_OCV, &calc_fops);

	debugfs_create_file("simultaneous", 0644, chip->dent,
			(void *)GET_VBAT_VSENSE_SIMULTANEOUS, &calc_fops);

	for (i = 0; i < ARRAY_SIZE(bms_irq_data); i++) {
		if (chip->pmic_bms_irq[bms_irq_data[i].irq_id])
			debugfs_create_file(bms_irq_data[i].name, 0444,
				chip->dent,
				(void *)bms_irq_data[i].irq_id,
				&rt_fops);
	}
}

#define REG_SBI_CONFIG		0x04F
#define PAGE3_ENABLE_MASK	0x6
#define PROGRAM_REV_MASK	0x0F
#define PROGRAM_REV		0x9
static int read_ocv_trim(struct pm8921_bms_chip *chip)
{
	int rc;
	u8 reg, sbi_config;

	rc = pm8xxx_readb(chip->dev->parent, REG_SBI_CONFIG, &sbi_config);
	if (rc) {
		pr_err("error = %d reading sbi config reg\n", rc);
		return rc;
	}

	reg = sbi_config | PAGE3_ENABLE_MASK;
	rc = pm8xxx_writeb(chip->dev->parent, REG_SBI_CONFIG, reg);
	if (rc) {
		pr_err("error = %d writing sbi config reg\n", rc);
		return rc;
	}

	rc = pm8xxx_readb(chip->dev->parent, TEST_PROGRAM_REV, &reg);
	if (rc)
		pr_err("Error %d reading %d addr %d\n",
			rc, reg, TEST_PROGRAM_REV);
	pr_err("program rev reg is 0x%x\n", reg);
	reg &= PROGRAM_REV_MASK;

	/* If the revision is equal or higher do not adjust trim delta */
	if (reg >= PROGRAM_REV) {
		chip->amux_2_trim_delta = 0;
		goto restore_sbi_config;
	}

	rc = pm8xxx_readb(chip->dev->parent, AMUX_TRIM_2, &reg);
	if (rc) {
		pr_err("error = %d reading trim reg\n", rc);
		return rc;
	}

	pr_err("trim reg is 0x%x\n", reg);
	chip->amux_2_trim_delta = abs(0x49 - reg);
	pr_err("trim delta is %d\n", chip->amux_2_trim_delta);

restore_sbi_config:
	rc = pm8xxx_writeb(chip->dev->parent, REG_SBI_CONFIG, sbi_config);
	if (rc) {
		pr_err("error = %d writing sbi config reg\n", rc);
		return rc;
	}

	return 0;
}

static ssize_t pm8921_bms_read_cc(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_cc_param);
	return ret;
}

static ssize_t pm8921_bms_read_last_good_ocv(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_last_good_ocv_param);
	return ret;
}

static ssize_t pm8921_bms_read_ocv_for_rbatt(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_ocv_for_rbatt_param);
	return ret;
}

static ssize_t pm8921_bms_read_vbatt_for_rbatt(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_vbatt_for_rbatt_param);
	return ret;
}

static ssize_t pm8921_bms_read_vsense_avg(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_vsense_avg_param);
	return ret;
}

static ssize_t pm8921_bms_read_vsense_for_rbatt(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_vsense_for_rbatt_param);
	return ret;
}

static ssize_t pm8921_bms_read_pc_unusable(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_pc_unusable_param);
	return ret;
}

static ssize_t pm8921_read_ratio_for_readjust_fcc(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n",
			read_ratio_for_readjust_fcc_param);
	return ret;
}

static ssize_t pm8921_read_total_ratio_for_readjust_fcc(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n",
			read_total_ratio_for_readjust_fcc_param);
	return ret;
}

static ssize_t pm8921_read_total_ratio_for_readjust_rbatt(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", ratio_to_default_rbatt);
	return ret;
}

static ssize_t pm8921_read_soc_original(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_soc_original_param);
	return ret;
}

static ssize_t pm8921_read_soc_expand(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", read_soc_expand_param);
	return ret;
}

static ssize_t pm8921_bms_read_i_test(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", the_chip->i_test);
	return ret;
}

static ssize_t pm8921_bms_write_i_test(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc;
	unsigned long arg;
	rc = strict_strtoul(buf, 10, &arg);
	if (!rc)
		the_chip->i_test = (unsigned int)arg;
	return strnlen(buf, PAGE_SIZE);
}

static struct device_attribute pm8921_bms_attrs[] = {
	__ATTR(read_cc, 0444, pm8921_bms_read_cc, NULL),
	__ATTR(read_last_good_ocv, 0444, pm8921_bms_read_last_good_ocv, NULL),
	__ATTR(read_ocv_for_rbatt, 0444, pm8921_bms_read_ocv_for_rbatt, NULL),
	__ATTR(read_vbatt_for_rbatt, 0444,
			pm8921_bms_read_vbatt_for_rbatt, NULL),
	__ATTR(read_vsense_avg, 0444, pm8921_bms_read_vsense_avg, NULL),
	__ATTR(read_vsense_for_rbatt, 0444,
			pm8921_bms_read_vsense_for_rbatt, NULL),
	__ATTR(read_pc_unusable, 0444, pm8921_bms_read_pc_unusable, NULL),
	__ATTR(read_ratio_for_readjust_fcc, 0444,
			pm8921_read_ratio_for_readjust_fcc, NULL),
	__ATTR(read_total_ratio_for_readjust_fcc, 0444,
			pm8921_read_total_ratio_for_readjust_fcc, NULL),
	__ATTR(read_total_ratio_for_readjust_rbatt, 0444,
			pm8921_read_total_ratio_for_readjust_rbatt, NULL),
	__ATTR(read_soc_original, 0444, pm8921_read_soc_original, NULL),
	__ATTR(read_soc_expand, 0444, pm8921_read_soc_expand, NULL),
	__ATTR(i_test, 0644, pm8921_bms_read_i_test, pm8921_bms_write_i_test),
};

static int create_sysfs_entries(struct pm8921_bms_chip *chip)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(pm8921_bms_attrs); i++) {
		rc = device_create_file(chip->dev, &pm8921_bms_attrs[i]);
		if (rc < 0)
			goto revert;
	}

	return 0;

revert:
	for (; i >= 0; i--)
		device_remove_file(chip->dev, &pm8921_bms_attrs[i]);

	return rc;
}

static void remove_sysfs_entries(struct pm8921_bms_chip *chip)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pm8921_bms_attrs); i++)
		device_remove_file(chip->dev, &pm8921_bms_attrs[i]);
}

static int pm8921_bms_suspend(struct device *dev)
{
	read_soc_expand_param = -EINVAL;
	return 0;
}

static int __devinit pm8921_bms_probe(struct platform_device *pdev)
{
	int rc = 0;
	int vbatt;
	struct pm8921_bms_chip *chip;
	const struct pm8921_bms_platform_data *pdata
				= pdev->dev.platform_data;

	if (!pdata) {
		pr_err("missing platform data\n");
		return -EINVAL;
	}

	chip = kzalloc(sizeof(struct pm8921_bms_chip), GFP_KERNEL);
	if (!chip) {
		pr_err("Cannot allocate pm_bms_chip\n");
		return -ENOMEM;
	}

	mutex_init(&chip->bms_output_lock);
	mutex_init(&chip->last_ocv_uv_mutex);
	chip->dev = &pdev->dev;
	chip->r_sense = pdata->r_sense;
	chip->i_test = pdata->i_test;
	chip->v_failure = pdata->v_failure;
	chip->max_voltage_uv = pdata->max_voltage_uv;
	chip->default_rbatt_mohms = pdata->default_rbatt_mohms;
	chip->batt_type = pdata->battery_type;
	chip->rconn_mohm = pdata->rconn_mohm;
	chip->start_percent = -EINVAL;
	chip->end_percent = -EINVAL;
	if (pdata->battery_data) {
		chip->fcc = pdata->battery_data->fcc;
		chip->fcc_temp_lut = pdata->battery_data->fcc_temp_lut;
		chip->fcc_sf_lut = pdata->battery_data->fcc_sf_lut;
		chip->pc_temp_ocv_lut = pdata->battery_data->pc_temp_ocv_lut;
		chip->pc_sf_lut = pdata->battery_data->pc_sf_lut;
		chip->rbatt_temp_soc_lut =
			pdata->battery_data->rbatt_temp_soc_lut;
	} else {
		rc = set_battery_data(chip);
		if (rc) {
			pr_err("%s bad battery data %d\n", __func__, rc);
			goto free_chip;
		}
	}

	if (chip->pc_temp_ocv_lut == NULL) {
		pr_err("temp ocv lut table is NULL\n");
		rc = -EINVAL;
		goto free_chip;
	}

	/* set defaults in the battery data */
	if (chip->default_rbatt_mohm <= 0)
		chip->default_rbatt_mohm = DEFAULT_RBATT_MOHMS;

	chip->batt_temp_channel = pdata->bms_cdata.batt_temp_channel;
	chip->vbat_channel = pdata->bms_cdata.vbat_channel;
	chip->ref625mv_channel = pdata->bms_cdata.ref625mv_channel;
	chip->ref1p25v_channel = pdata->bms_cdata.ref1p25v_channel;
	chip->batt_id_channel = pdata->bms_cdata.batt_id_channel;
	chip->revision = pm8xxx_get_revision(chip->dev->parent);
	chip->enable_fcc_learning = pdata->enable_fcc_learning;
	INIT_WORK(&chip->calib_hkadc_work, calibrate_hkadc_work);

	rc = request_irqs(chip, pdev);
	if (rc) {
		pr_err("couldn't register interrupts rc = %d\n", rc);
		goto free_chip;
	}

	rc = pm8921_bms_hw_init(chip);
	if (rc) {
		pr_err("couldn't init hardware rc = %d\n", rc);
		goto free_irqs;
	}

	read_shutdown_soc(chip);

	platform_set_drvdata(pdev, chip);
	the_chip = chip;
	create_debugfs_entries(chip);
	rc = create_sysfs_entries(chip);
	if (rc < 0)
		pr_err("sysfs create failed rc = %d\n", rc);

	rc = read_ocv_trim(chip);
	if (rc) {
		pr_err("couldn't adjust ocv_trim rc= %d\n", rc);
		goto free_irqs;
	}
	check_initial_ocv(chip);

	/* initial hkadc calibration */
	schedule_work(&chip->calib_hkadc_work);
	/* enable the vbatt reading interrupts for scheduling hkadc calib */
	pm8921_bms_enable_irq(chip, PM8921_BMS_GOOD_OCV);
	pm8921_bms_enable_irq(chip, PM8921_BMS_OCV_FOR_R);

	INIT_DELAYED_WORK(&chip->uuc_timer_work, uuc_timer_work);
	schedule_delayed_work(&chip->uuc_timer_work,
					msecs_to_jiffies(UUC_TIMER_MS));

	if (!get_battery_uvolts(chip, &vbatt))
		pr_info("OK battery_capacity_at_boot=%d volt = %d ocv = %d\n",
				pm8921_bms_get_percent_charge(),
				vbatt, last_ocv_uv);
	else
		pr_info("couldn't get battery voltage at boot\n");
	return 0;

free_irqs:
	free_irqs(chip);
free_chip:
	kfree(chip);
	return rc;
}

static int __devexit pm8921_bms_remove(struct platform_device *pdev)
{
	struct pm8921_bms_chip *chip = platform_get_drvdata(pdev);

	remove_sysfs_entries(chip);
	free_irqs(chip);
	kfree(chip->adjusted_fcc_temp_lut);
	platform_set_drvdata(pdev, NULL);
	the_chip = NULL;
	kfree(chip);
	return 0;
}

static const struct dev_pm_ops pm8921_pm_ops = {
	.suspend	= pm8921_bms_suspend,
};

static struct platform_driver pm8921_bms_driver = {
	.probe	= pm8921_bms_probe,
	.remove	= __devexit_p(pm8921_bms_remove),
	.driver	= {
		.name	= PM8921_BMS_DEV_NAME,
		.owner	= THIS_MODULE,
		.pm	= &pm8921_pm_ops
	},
};

static int __init pm8921_bms_init(void)
{
	return platform_driver_register(&pm8921_bms_driver);
}

static void __exit pm8921_bms_exit(void)
{
	platform_driver_unregister(&pm8921_bms_driver);
}

late_initcall(pm8921_bms_init);
module_exit(pm8921_bms_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("PMIC8921 bms driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:" PM8921_BMS_DEV_NAME);
