/* Copyright (c) 2013-2014, LG Eletronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "[LGE-CC] %s : " fmt, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/wakelock.h>
#include <linux/power_supply.h>

#include <soc/qcom/lge/lge_charging_scenario.h>
#include <soc/qcom/lge/power/lge_power_class.h>
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
#include <linux/alarmtimer.h>
#endif
#ifdef CONFIG_LGE_PM_PSEUDO_BATTERY
#include <soc/qcom/lge/lge_pseudo_batt.h>
#endif
#include <soc/qcom/lge/board_lge.h>


#define MODULE_NAME "lge_charging_controller"
#define MONITOR_BATTEMP_POLLING_PERIOD  (60 * HZ)
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
#define BTM_ALARM_TIME(DELAY) (DELAY##LL * NSEC_PER_SEC)
#define BTM_ALARM_PERIOD BTM_ALARM_TIME(60) /* 60sec */
#endif
#define RESTRICTED_CHG_CURRENT_500  500
#define RESTRICTED_CHG_CURRENT_300  300
#define CHG_CURRENT_MAX 3100

struct lge_charging_controller {
	struct device 			*dev;
	struct power_supply		*batt_psy;
	struct power_supply		*usb_psy;
	struct power_supply		*bms_psy;
	struct lge_power 		lge_cc_lpc;
	struct lge_power 		*lge_cd_lpc;

	struct delayed_work 	battemp_work;
	struct wake_lock 		lcs_wake_lock;
#ifndef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	struct wake_lock 		chg_wake_lock;
#endif

	enum lge_charging_states battemp_chg_state;

	int chg_current_te;
	int chg_current_max;
	int otp_ibat_current;
	int iusb_current;
	int ibat_current;
	int ta_current;
	int usb_current;
	int pseudo_chg_ui;
	int before_battemp;
	int batt_temp;
	int btm_state;
	int start_batt_temp;
	int stop_batt_temp;
	int chg_enable;
	int test_chg_scenario;
	int test_batt_therm_value;
	int is_usb_present;
	int chg_type;
	int quick_chg_status;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	int is_hvdcp_present;
	int update_hvdcp_state;
	int finish_hvdcp_set_cur;
	struct delayed_work hvdcp_set_cur_work;
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	struct power_supply	*ctype_psy;
	struct delayed_work ctype_detect_work;
	int 		ctype_present;
	int 		ctype_type;
	int update_ctype_state;
	int finish_check_ctype;
#endif
	bool quick_set_enable;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	struct power_supply	*chg_psy;
	struct alarm btm_polling_alarm;
	struct wake_lock alarm_wake_lock;
	u8 btm_alarm_enable:1;
	u8 pre_btm_alarm_enable:1;
	bool charger_eoc;
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	int tdmb_mode_on;
#endif
	int chg_done;
};

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
enum qpnp_quick_charging_status {
	HVDCP_STATUS_NONE = 0,
	HVDCP_STATUS_LCD_ON,
	HVDCP_STATUS_LCD_OFF,
	HVDCP_STATUS_CALL_ON,
	HVDCP_STATUS_CALL_OFF,
};
#endif

static enum lge_power_property lge_power_lge_cc_properties[] = {
	LGE_POWER_PROP_PSEUDO_BATT_UI,
	LGE_POWER_PROP_BTM_STATE,
	LGE_POWER_PROP_CHARGING_ENABLED,
	LGE_POWER_PROP_INPUT_CURRENT_MAX,
	LGE_POWER_PROP_OTP_CURRENT,
	LGE_POWER_PROP_TEST_CHG_SCENARIO,
	LGE_POWER_PROP_TEST_BATT_THERM_VALUE,
	LGE_POWER_PROP_TEST_CHG_SCENARIO,
#ifdef CONFIG_LGE_PM_PSEUDO_BATTERY
	LGE_POWER_PROP_PSEUDO_BATT,
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	LGE_POWER_PROP_TDMB_MODE_ON,
#endif
	LGE_POWER_PROP_CHARGE_DONE,
	LGE_POWER_PROP_TYPE,
};

static char *lge_cc_supplied_to[] = {
	"battery",
};

static char *lge_cc_supplied_from[] = {
	"lge_cable_detect",
};

static struct lge_charging_controller *the_cc;

enum lgcc_vote_reason {
	LGCC_REASON_DEFAULT,
	LGCC_REASON_OTP,
	LGCC_REASON_LCD,
	LGCC_REASON_CALL,
	LGCC_REASON_THERMAL,
	LGCC_REASON_THERMAL_HVDCP,
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	LGCC_REASON_TDMB,
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	LGCC_REASON_HVDCP,
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	LGCC_REASON_CTYPE,
#endif
	LGCC_REASON_PIF,
	LGCC_REASON_MAX,
};

static int lgcc_vote_fcc_table[LGCC_REASON_MAX] = {
	CHG_CURRENT_MAX,	/* max ibat current */
	-EINVAL,
	-EINVAL,
	-EINVAL,
	-EINVAL,
	-EINVAL,
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	-EINVAL,
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	-EINVAL,
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	-EINVAL,
#endif
	-EINVAL,
};

static int lgcc_vote_fcc_reason = -EINVAL;
static void lgcc_vote_fcc_update(void)
{
	int fcc = INT_MAX;
	int reason = -EINVAL;
	int i;

	for (i = 0; i < LGCC_REASON_MAX; i++) {
		if (lgcc_vote_fcc_table[i] == -EINVAL)
			continue;

		if (fcc > lgcc_vote_fcc_table[i]) {
			fcc = lgcc_vote_fcc_table[i];
			reason = i;
		}
	}

	if (!the_cc)
		return;

	if (reason != lgcc_vote_fcc_reason) {
		lgcc_vote_fcc_reason = reason;
		pr_info("lgcc_vote: vote id[%d], set cur[%d]\n",
				reason, fcc);
		lge_power_changed(&the_cc->lge_cc_lpc);
	}
}

static int lgcc_vote_fcc(int reason, int fcc)
{
	lgcc_vote_fcc_table[reason] = fcc;
	lgcc_vote_fcc_update();

	return 0;
}

static int lgcc_vote_fcc_get(void)
{
	if (lgcc_vote_fcc_reason == -EINVAL)
		return -EINVAL;

	return lgcc_vote_fcc_table[lgcc_vote_fcc_reason];
}

static int check_hvdcp_type(struct lge_charging_controller *cc) {
	union lge_power_propval lge_val = {0,};
	int rc;

	if (!cc->lge_cd_lpc)
		cc->lge_cd_lpc = lge_power_get_by_name("lge_cable_detect");
	if (cc->lge_cd_lpc) {
		rc = cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
				LGE_POWER_PROP_TYPE, &lge_val);
		if (lge_val.intval == POWER_SUPPLY_TYPE_USB_HVDCP ||
				lge_val.intval == POWER_SUPPLY_TYPE_USB_HVDCP_3)
			return 1;
	}

	return 0;
}

static int lgcc_thermal_mitigation;
static int lgcc_set_thermal_chg_current(const char *val,
		struct kernel_param *kp) {

	int ret;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	if (!the_cc) {
		pr_err("lgcc is not ready\n");
		return 0;
	}

	the_cc->is_hvdcp_present = check_hvdcp_type(the_cc);

	if (lgcc_thermal_mitigation > 0 && lgcc_thermal_mitigation < 1500) {
		the_cc->chg_current_te = lgcc_thermal_mitigation;
		lgcc_vote_fcc(LGCC_REASON_THERMAL, lgcc_thermal_mitigation);
	} else {
		pr_info("Released thermal mitigation\n");
		the_cc->chg_current_te = CHG_CURRENT_MAX;
		lgcc_vote_fcc(LGCC_REASON_THERMAL, -EINVAL);
	}

	pr_info("thermal_mitigation = %d, chg_current_te = %d\n",
			lgcc_thermal_mitigation,
			the_cc->chg_current_te);

	cancel_delayed_work_sync(&the_cc->battemp_work);
	schedule_delayed_work(&the_cc->battemp_work, HZ*1);

	return 0;
}
module_param_call(lgcc_thermal_mitigation,
		lgcc_set_thermal_chg_current,
		param_get_int, &lgcc_thermal_mitigation, 0644);

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
static int lgcc_hvdcp_thermal_mitigation;
static int lgcc_set_hvdcp_thermal_chg_current(const char *val,
		struct kernel_param *kp) {

	int ret;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	if (!the_cc) {
		pr_err("lgcc is not ready\n");
		return 0;
	}

	if (lgcc_hvdcp_thermal_mitigation > 0 && lgcc_hvdcp_thermal_mitigation < 2000) {
		the_cc->chg_current_te = lgcc_hvdcp_thermal_mitigation;
		lgcc_vote_fcc(LGCC_REASON_THERMAL_HVDCP, lgcc_hvdcp_thermal_mitigation);
	} else {
		pr_info("Released thermal mitigation\n");
		the_cc->chg_current_te = CHG_CURRENT_MAX;
		lgcc_vote_fcc(LGCC_REASON_THERMAL_HVDCP, -EINVAL);
	}
	pr_err("thermal_mitigation = %d, chg_current_te = %d\n",
			lgcc_hvdcp_thermal_mitigation,
			the_cc->chg_current_te);

	cancel_delayed_work_sync(&the_cc->battemp_work);
	schedule_delayed_work(&the_cc->battemp_work, HZ*1);

	return 0;
}
module_param_call(lgcc_hvdcp_thermal_mitigation,
		lgcc_set_hvdcp_thermal_chg_current,
		param_get_int, &lgcc_hvdcp_thermal_mitigation, 0644);
#endif

static int lgcc_iusb_control = 0;
static int lgcc_set_iusb_control(const char *val,
		struct kernel_param *kp){
	int ret;
	union power_supply_propval pval = {0,};

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	} else if (!the_cc) {
		pr_err("lgcc is not ready\n");
		return 0;
	} else if (lgcc_thermal_mitigation < 0) {
		pr_err("invalid value setting\n");
		return 0;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	} else if (lgcc_hvdcp_thermal_mitigation < 0) {
		pr_err("invalid value setting\n");
		return 0;
#endif
	}

	pr_info("set lgcc_iusb_control to %d\n", lgcc_iusb_control);

	pval.intval = lgcc_iusb_control;
	the_cc->batt_psy->set_property(the_cc->batt_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &pval);

	return 0;
}
module_param_call(lgcc_iusb_control, lgcc_set_iusb_control,
		param_get_int, &lgcc_iusb_control, 0644);

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
#define NORMAL_CHG_CURRENT_MAX 3100
#define RESTRICTED_CALL_STATE 500
#define RESTRICTED_LCD_STATE 1000
static int quick_charging_state;
static int set_quick_charging_state(const char *val,
		struct kernel_param *kp) {
	int ret;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_info("quick_charging_state error = %d\n", ret);
		return ret;
	}

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	if (the_cc->tdmb_mode_on) {
		pr_info("Enter tdmb mode\n");
		return 0;
	}
#endif

	the_cc->quick_chg_status = quick_charging_state;

	switch (quick_charging_state) {
		case HVDCP_STATUS_LCD_ON:
			lgcc_vote_fcc(LGCC_REASON_LCD, RESTRICTED_LCD_STATE);
			pr_info("LCD on decreasing chg_current\n");
			break;

		case HVDCP_STATUS_LCD_OFF:
			lgcc_vote_fcc(LGCC_REASON_LCD, -EINVAL);
			pr_debug("LCD off return max chg_current\n");
			break;

		case HVDCP_STATUS_CALL_ON:
			lgcc_vote_fcc(LGCC_REASON_CALL, RESTRICTED_CALL_STATE);
			pr_info("Call on  decreasing chg_current\n");
			break;

		case HVDCP_STATUS_CALL_OFF:
			lgcc_vote_fcc(LGCC_REASON_CALL, -EINVAL);
			pr_debug("Call off return max chg_current\n");
			break;

		default:
			lgcc_vote_fcc(LGCC_REASON_LCD, -EINVAL);
			lgcc_vote_fcc(LGCC_REASON_CALL, -EINVAL);
			break;
	}

	pr_info("set quick_charging_state[%d]\n", quick_charging_state);

	lge_power_changed(&the_cc->lge_cc_lpc);
	return 0;
}
module_param_call(quick_charging_state, set_quick_charging_state,
		param_get_int, &quick_charging_state, 0644);
#endif

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
static void lgcc_btm_set_polling_alarm(struct lge_charging_controller *cc, u64 delay)
{
	ktime_t kt = ns_to_ktime(delay);

	if (cc->btm_alarm_enable)
		alarm_start_relative(&cc->btm_polling_alarm, kt);
}

static enum alarmtimer_restart lgcc_btm_alarm(struct alarm *alarm, ktime_t kt)
{
	struct lge_charging_controller *cc = container_of(alarm,
			struct lge_charging_controller, btm_polling_alarm);

	if (cc->btm_alarm_enable)
		wake_lock(&cc->alarm_wake_lock);

	schedule_delayed_work(&cc->battemp_work, msecs_to_jiffies(50));

	return ALARMTIMER_NORESTART;
}
#endif

static void lge_monitor_batt_temp_work(struct work_struct *work){

	struct charging_info req;
	struct charging_rsp res;
	bool is_changed = false;
	union power_supply_propval ret = {0,};
#if 0//def CONFIG_LGE_PM_LGE_POWER_CLASS_CABLE_DETECT
	union lge_power_propval lge_val = {0,};
#endif
	struct lge_charging_controller *cc =
		container_of(work, struct lge_charging_controller,
			battemp_work.work);

	cc->usb_psy = power_supply_get_by_name("usb");

	if(!cc->usb_psy){
		pr_err("usb power_supply not found deferring probe\n");
		schedule_delayed_work(&cc->battemp_work,
			MONITOR_BATTEMP_POLLING_PERIOD);
		return;
	}

	cc->batt_psy = power_supply_get_by_name("battery");

	if(!cc->batt_psy){
		pr_err("battery power_supply not found deferring probe\n");
		schedule_delayed_work(&cc->battemp_work,
			MONITOR_BATTEMP_POLLING_PERIOD);
		return;
	}

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CABLE_DETECT
	cc->lge_cd_lpc = lge_power_get_by_name("lge_cable_detect");
	if (!cc->lge_cd_lpc) {
		pr_err("lge_cd_lpc is not yet ready\n");
		schedule_delayed_work(&cc->battemp_work,
			MONITOR_BATTEMP_POLLING_PERIOD);
		return;
	}
#endif
	cc->batt_psy->get_property(cc->batt_psy,
			POWER_SUPPLY_PROP_TEMP, &ret);
	if (cc->test_chg_scenario == 1)
		req.batt_temp = cc->test_batt_therm_value;
	else
		req.batt_temp = ret.intval / 10;
	cc->batt_temp = req.batt_temp;

	cc->batt_psy->get_property(cc->batt_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &ret);
	req.batt_volt = ret.intval;

	cc->batt_psy->get_property(cc->batt_psy,
			POWER_SUPPLY_PROP_CURRENT_NOW, &ret);
	req.current_now = ret.intval / 1000;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CABLE_DETECT
//	cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
//			LGE_POWER_PROP_CHARGING_CURRENT_MAX, &lge_val);
//	cc->chg_current_max = lge_val.intval / 1000;
	cc->chg_current_max = cc->ibat_current / 1000;
#else
	cc->usb_psy->get_property(
			cc->usb_psy, POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
	cc->chg_current_max = ret.intval / 1000;
#endif
	req.chg_current_ma = cc->chg_current_max;

	if (cc->chg_current_te != -EINVAL)
		req.chg_current_te = cc->chg_current_te;
	else
		req.chg_current_te = cc->chg_current_max;

	pr_debug("chg_curren_te = %d\n", cc->chg_current_te);
	cc->usb_psy->get_property(cc->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &ret);
	req.is_charger = ret.intval;

	lge_monitor_batt_temp(req, &res);

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	cc->batt_psy->get_property(cc->batt_psy,
			POWER_SUPPLY_PROP_CAPACITY, &ret);
	if (ret.intval == 100)
		cc->charger_eoc = 1;
	else
		cc->charger_eoc = 0;

	cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
			LGE_POWER_PROP_CHG_PRESENT, &lge_val);
	cc->btm_alarm_enable = (lge_val.intval && !cc->charger_eoc);
	pr_err("(%s)->(%s)\n",
			cc->pre_btm_alarm_enable ? "alarm":"work_queue",
			cc->btm_alarm_enable ? "alarm":"work_queue");
	cc->pre_btm_alarm_enable = cc->btm_alarm_enable;
#endif
	if (((res.change_lvl != STS_CHE_NONE) && req.is_charger) ||
			(res.force_update == true)) {
		if (res.change_lvl == STS_CHE_NORMAL_TO_DECCUR ||
				(res.state == CHG_BATT_DECCUR_STATE &&
				 res.dc_current != DC_CURRENT_DEF &&
				 res.change_lvl != STS_CHE_STPCHG_TO_DECCUR)) {
			pr_info("ibatmax_set STS_CHE_NORMAL_TO_DECCUR\n");
			cc->otp_ibat_current = res.dc_current;
			cc->chg_enable = 1;
			lgcc_vote_fcc(LGCC_REASON_OTP, res.dc_current);
		} else if (res.change_lvl == STS_CHE_NORMAL_TO_STPCHG ||
				(res.state == CHG_BATT_STPCHG_STATE)) {
			pr_info("ibatmax_set STS_CHE_NORMAL_TO_STPCHG\n");
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
			if (!cc->btm_alarm_enable)
				wake_lock(&cc->lcs_wake_lock);
#else
			wake_lock(&cc->lcs_wake_lock);
#endif
			cc->otp_ibat_current = 0;
			cc->chg_enable = 0;
			lgcc_vote_fcc(LGCC_REASON_OTP, 0);
		} else if (res.change_lvl == STS_CHE_DECCUR_TO_NORAML) {
			pr_info("ibatmax_set STS_CHE_DECCUR_TO_NORAML\n");
			cc->otp_ibat_current = res.dc_current;
			cc->chg_enable = 1;
			lgcc_vote_fcc(LGCC_REASON_OTP, -EINVAL);
		} else if (res.change_lvl == STS_CHE_DECCUR_TO_STPCHG) {
			pr_info("ibatmax_set STS_CHE_DECCUR_TO_STPCHG\n");
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
			if (!cc->btm_alarm_enable)
				wake_lock(&cc->lcs_wake_lock);
#else
			wake_lock(&cc->lcs_wake_lock);
#endif
			cc->otp_ibat_current = 0;
			cc->chg_enable = 0;
			lgcc_vote_fcc(LGCC_REASON_OTP, 0);
		} else if (res.change_lvl == STS_CHE_STPCHG_TO_NORMAL) {
			pr_info("ibatmax_set STS_CHE_STPCHG_TO_NORMAL\n");
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
			if (!cc->btm_alarm_enable)
				wake_unlock(&cc->lcs_wake_lock);
#else
			wake_unlock(&cc->lcs_wake_lock);
#endif
			cc->otp_ibat_current = res.dc_current;
			cc->chg_enable = 1;
			lgcc_vote_fcc(LGCC_REASON_OTP, -EINVAL);
		} else if (res.change_lvl == STS_CHE_STPCHG_TO_DECCUR) {
			pr_info("ibatmax_set STS_CHE_STPCHG_TO_DECCUR\n");
			cc->otp_ibat_current = res.dc_current;
			cc->chg_enable = 1;
			lgcc_vote_fcc(LGCC_REASON_OTP, res.dc_current);
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
			if (!cc->btm_alarm_enable)
				wake_unlock(&cc->lcs_wake_lock);
#else
			wake_unlock(&cc->lcs_wake_lock);
#endif

		} else if (res.force_update == true &&
				res.state == CHG_BATT_NORMAL_STATE &&
				res.dc_current != DC_CURRENT_DEF) {
			pr_info("ibatmax_set CHG_BATT_NORMAL_STATE\n");
			cc->otp_ibat_current = res.dc_current;
			cc->chg_enable = 1;
			lgcc_vote_fcc(LGCC_REASON_OTP, -EINVAL);
		}
	}

	if (cc->chg_current_te == 0) {
		pr_info("thermal_miti ibat < 300, stop charging!\n");
		cc->otp_ibat_current = 0;
		cc->chg_enable = 0;
//		lge_power_changed(&cc->lge_cc_lpc);
	} else if (cc->chg_current_te == cc->chg_current_max)
		lgcc_vote_fcc(LGCC_REASON_THERMAL, -EINVAL);

	if (cc->quick_chg_status == 1)
		lgcc_vote_fcc(LGCC_REASON_LCD, RESTRICTED_LCD_STATE);
	else if (cc->quick_chg_status == 3)
		lgcc_vote_fcc(LGCC_REASON_CALL, RESTRICTED_CALL_STATE);
	else {
		lgcc_vote_fcc(LGCC_REASON_LCD, -EINVAL);
		lgcc_vote_fcc(LGCC_REASON_CALL, -EINVAL);
	}
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
	if (cc->tdmb_mode_on == 1)
		lgcc_vote_fcc(LGCC_REASON_TDMB, RESTRICTED_CHG_CURRENT_500);
	else if (cc->tdmb_mode_on == 2)
		lgcc_vote_fcc(LGCC_REASON_TDMB, RESTRICTED_CHG_CURRENT_300);
	else if (cc->tdmb_mode_on == 0)
		lgcc_vote_fcc(LGCC_REASON_TDMB, -EINVAL);
#endif

	pr_info("otp_ibat_current=%d\n", cc->otp_ibat_current);

	pr_debug("cc->pseudo_chg_ui = %d, res.pseudo_chg_ui = %d\n",
			cc->pseudo_chg_ui, res.pseudo_chg_ui);

	if (cc->pseudo_chg_ui ^ res.pseudo_chg_ui) {
		is_changed = true;
		cc->pseudo_chg_ui = res.pseudo_chg_ui;
	}

	pr_debug("cc->btm_state = %d, res.btm_state = %d\n",
			cc->btm_state, res.btm_state);
	if (cc->btm_state ^ res.btm_state) {
		is_changed = true;
		cc->btm_state = res.btm_state;
	}

	if (cc->before_battemp != req.batt_temp) {
		is_changed = true;
		cc->before_battemp = req.batt_temp;
	}

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	if (cc->update_hvdcp_state == 1){
		is_changed = true;
		cc->update_hvdcp_state = 0;
	}
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	if (cc->update_ctype_state == 1) {
		is_changed = true;
		cc->update_ctype_state = 0;
	}
#endif

	cc->batt_psy->get_property(cc->batt_psy,
			POWER_SUPPLY_PROP_CAPACITY, &ret);
#ifndef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	pr_debug("cap : %d, hvdcp : %d, usb : %d\n",
			ret.intval, cc->is_hvdcp_present, cc->is_usb_present);

	if (!cc->is_usb_present	||
			(cc->is_usb_present	&& cc->chg_done)) {
		if (wake_lock_active(&cc->chg_wake_lock)) {
			pr_info("chg_wake_unlocked\n");
			wake_unlock(&cc->chg_wake_lock);
		}
	} else if (cc->is_usb_present && ret.intval < 100) {
		if (!wake_lock_active(&cc->chg_wake_lock)) {
			pr_info("chg_wake_locked\n");
			wake_lock(&cc->chg_wake_lock);
		}
	}
#endif
	if (is_changed == true)
		lge_power_changed(&cc->lge_cc_lpc);

	pr_info("Reported Capacity : %d / voltage : %d\n",
			ret.intval, req.batt_volt/1000);


	//	lgcc_charger_reginfo();
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	if (cc->btm_alarm_enable) {
		lgcc_btm_set_polling_alarm(cc, BTM_ALARM_PERIOD);

		if (wake_lock_active(&cc->lcs_wake_lock))
			wake_unlock(&cc->lcs_wake_lock);
	} else {
		if (cc->batt_temp <= 0)
			schedule_delayed_work(&cc->battemp_work,
					MONITOR_BATTEMP_POLLING_PERIOD / 6);
		else if (cc->batt_temp >= 450 && cc->batt_temp <= 550)
			schedule_delayed_work(&cc->battemp_work,
					MONITOR_BATTEMP_POLLING_PERIOD / 3);
		else if (cc->batt_temp >= 550 && cc->batt_temp <= 590)
			schedule_delayed_work(&cc->battemp_work,
					MONITOR_BATTEMP_POLLING_PERIOD / 6);
		else
			schedule_delayed_work(&cc->battemp_work,
					MONITOR_BATTEMP_POLLING_PERIOD);
	}

	if (wake_lock_active(&cc->alarm_wake_lock))
		wake_unlock(&cc->alarm_wake_lock);
#else
	if (cc->batt_temp <= 0)
		schedule_delayed_work(&cc->battemp_work,
				MONITOR_BATTEMP_POLLING_PERIOD / 6);
	else if (cc->batt_temp >= 450 && cc->batt_temp <= 550)
		schedule_delayed_work(&cc->battemp_work,
				MONITOR_BATTEMP_POLLING_PERIOD / 3);
	else if (cc->batt_temp >= 550 && cc->batt_temp <= 590)
		schedule_delayed_work(&cc->battemp_work,
				MONITOR_BATTEMP_POLLING_PERIOD / 6);
	else
		schedule_delayed_work(&cc->battemp_work,
				MONITOR_BATTEMP_POLLING_PERIOD);
#endif
}

static int lg_cc_get_pseudo_ui(struct lge_charging_controller *cc) {
	if(!(cc == NULL)) {
		return cc->pseudo_chg_ui;
	}

	return 0;
}

static int lg_cc_get_btm_state(struct lge_charging_controller *cc) {
	if(!(cc == NULL)) {
		return cc->btm_state;
	}

	return 0;
}

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
static void	lg_cc_start_battemp_alarm (struct lge_charging_controller *cc,
		u64 delay) {
	pr_debug("start_battemp_alarm~!!\n");

	if (!cc->btm_alarm_enable)
		cc->btm_alarm_enable = 1;

	lgcc_btm_set_polling_alarm(cc, delay);
}

static void lg_cc_stop_battemp_alarm(struct lge_charging_controller *cc) {
	pr_debug("stop_battemp_alarm~!!\n");

	if (cc->btm_alarm_enable)
		cc->btm_alarm_enable = 0;

	if (wake_lock_active(&cc->alarm_wake_lock))
		wake_unlock(&cc->alarm_wake_lock);

	cancel_delayed_work(&cc->battemp_work);
	alarm_cancel(&cc->btm_polling_alarm);
}
#endif

static void lg_cc_start_battemp_work(struct lge_charging_controller *cc,
		int delay) {
	pr_debug("start_battemp_work~!!\n");
	schedule_delayed_work(&cc->battemp_work, (delay * HZ));
}


#ifndef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
static void lg_cc_stop_battemp_work(struct lge_charging_controller *cc) {
	pr_debug("stop_battemp_work~!!\n");
	cancel_delayed_work(&cc->battemp_work);
}
#endif

static int lge_power_lge_cc_property_is_writeable(struct lge_power *lpc,
		enum lge_power_property lpp) {
	int ret = 0;
	switch (lpp) {
		case LGE_POWER_PROP_TEST_CHG_SCENARIO:
		case LGE_POWER_PROP_TEST_BATT_THERM_VALUE:
#ifdef CONFIG_LGE_PM_PSEUDO_BATTERY
		case LGE_POWER_PROP_PSEUDO_BATT:
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
		case LGE_POWER_PROP_TDMB_MODE_ON:
#endif
			ret = 1;
			break;
		default:
			break;
	}

	return ret;
}

static int lge_power_lge_cc_set_property(struct lge_power *lpc,
		enum lge_power_property lpp,
		const union lge_power_propval *val) {
	int ret_val = 0;
#ifdef TEMP_BLOCK
	union lge_power_propval lge_val = {0,};
#endif
	struct lge_charging_controller *cc
		= container_of(lpc,	struct lge_charging_controller,
				lge_cc_lpc);

	switch (lpp) {
		case LGE_POWER_PROP_TEST_CHG_SCENARIO:
			cc->test_chg_scenario = val->intval;
			break;

		case LGE_POWER_PROP_TEST_BATT_THERM_VALUE:
			cc->test_batt_therm_value = val->intval;
			break;

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
		case LGE_POWER_PROP_TDMB_MODE_ON:
			cc->tdmb_mode_on = val->intval;
			pr_info("tdmb mode is set to [%d]\n", cc->tdmb_mode_on);

			if (cc->tdmb_mode_on == 1)
				lgcc_vote_fcc(LGCC_REASON_TDMB, RESTRICTED_CHG_CURRENT_500);
			else if (cc->tdmb_mode_on == 2)
				lgcc_vote_fcc(LGCC_REASON_TDMB, RESTRICTED_CHG_CURRENT_300);
			else if (cc->tdmb_mode_on == 0)
				lgcc_vote_fcc(LGCC_REASON_TDMB, -EINVAL);
			break;
#endif
		case LGE_POWER_PROP_TYPE:
			cc->chg_type = val->intval;
			break;

		case LGE_POWER_PROP_CHARGE_DONE:
			cc->chg_done = val->intval;
			if (cc->chg_done) {
				if (delayed_work_pending(&cc->hvdcp_set_cur_work))
					cancel_delayed_work_sync(&cc->hvdcp_set_cur_work);
				if (delayed_work_pending(&cc->ctype_detect_work))
					cancel_delayed_work_sync(&cc->ctype_detect_work);
			}
			break;

		default:
			pr_err("lpp:%d is not supported!!!\n", lpp);
			ret_val = -EINVAL;
			break;
	}
	lge_power_changed(&cc->lge_cc_lpc);

	return ret_val;
}

static int lge_power_lge_cc_get_property(struct lge_power *lpc,
		enum lge_power_property lpp,
		union lge_power_propval *val) {
	int ret_val = 0;

	struct lge_charging_controller *cc
		= container_of(lpc, struct lge_charging_controller,
				lge_cc_lpc);
	switch (lpp) {
		case LGE_POWER_PROP_PSEUDO_BATT_UI:
			val->intval = lg_cc_get_pseudo_ui(cc);
			break;

		case LGE_POWER_PROP_BTM_STATE:
			val->intval = lg_cc_get_btm_state(cc);
			break;

		case LGE_POWER_PROP_CHARGING_ENABLED:
			val->intval = cc->chg_enable;
			break;

		case LGE_POWER_PROP_INPUT_CURRENT_MAX:
			val->intval = cc->iusb_current;
			break;

		case LGE_POWER_PROP_OTP_CURRENT:
			val->intval = lgcc_vote_fcc_get();//cc->otp_ibat_current;
			break;

		case LGE_POWER_PROP_TEST_CHG_SCENARIO:
			val->intval = cc->test_chg_scenario;
			break;

		case LGE_POWER_PROP_TEST_BATT_THERM_VALUE:
			val->intval = cc->test_batt_therm_value;
			break;

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TDMB_MODE
		case LGE_POWER_PROP_TDMB_MODE_ON:
			val->intval = cc->tdmb_mode_on;
			break;
#endif

#ifdef CONFIG_LGE_PM_PSEUDO_BATTERY
		case LGE_POWER_PROP_PSEUDO_BATT:
			val->intval = get_pseudo_batt_info(PSEUDO_BATT_MODE);
			break;
#endif
		case LGE_POWER_PROP_CHARGE_DONE:
			val->intval = cc->chg_done;
			break;

		case LGE_POWER_PROP_TYPE:
			val->intval = cc->chg_type;
			break;

		default:
			ret_val = -EINVAL;
			break;
	}

	return ret_val;
}

#ifdef CONFIG_LGE_USB_TYPE_C
#define CTYPE_CHECK_DELAY  (5 * 100)
#define CTYPE_IUSB_MAX 3000
#define CTYPE_IBAT_MAX 3000
#define CTYPE_IBAT_DEFAULT 1500
static void lge_check_typec_work(struct work_struct *work) {
	struct lge_charging_controller *cc =
		container_of(work, struct lge_charging_controller,
				ctype_detect_work.work);
	union power_supply_propval pval = {0, };
	union lge_power_propval lge_val = {0, };
	int rc = 0;
	static int counter;
	static int ibat_temp;

	pr_info("Check c-type cable\n");

   cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
		   LGE_POWER_PROP_CHARGING_CURRENT_MAX, &lge_val);
   ibat_temp = lge_val.intval;

	if (cc->is_usb_present) {
		if (!cc->finish_check_ctype && !cc->is_hvdcp_present)
			cc->ibat_current = CTYPE_IBAT_DEFAULT;
	} else {
		counter = 0;
		ibat_temp = 0;
		lgcc_vote_fcc(LGCC_REASON_CTYPE, -EINVAL);
	}
	if (!cc->ctype_psy) {
		cc->ctype_psy = power_supply_get_by_name("usb_pd");
		cc->ctype_type = 0;
		if (!cc->ctype_psy)
			goto skip_ctype;
	} else {
		rc = cc->ctype_psy->get_property(cc->ctype_psy,
				POWER_SUPPLY_PROP_TYPE, &pval);
		if (rc == 0)
			cc->ctype_type = pval.intval;
		else
			pr_err("Failed to get usb_pd property\n");
	}

	if (!(cc->ctype_type == POWER_SUPPLY_TYPE_CTYPE_PD ||
				cc->ctype_type == POWER_SUPPLY_TYPE_CTYPE)) {
		counter++;
		if (counter > 5) {
			cc->finish_check_ctype = 1;
			lgcc_vote_fcc(LGCC_REASON_CTYPE, -EINVAL);
			counter = 0;
			pr_err("Not type-C\n");
		}
	} else {
		rc = cc->ctype_psy->get_property(cc->ctype_psy,
				POWER_SUPPLY_PROP_CURRENT_MAX, &pval);
		if (rc == 0)
			ibat_temp = pval.intval / 1000;
		else
			pr_err("Failed to get usb_pd property\n");

		cc->finish_check_ctype = 1;
		cc->ctype_present = 1;
		if (delayed_work_pending(&cc->hvdcp_set_cur_work))
			cancel_delayed_work_sync(&cc->hvdcp_set_cur_work);
	}

	if (cc->ctype_present && cc->finish_check_ctype) {
		pr_info("Detect c-type cable\n");
		cc->ibat_current = ibat_temp;
		cc->iusb_current = cc->usb_current * 1000;
		cc->otp_ibat_current = cc->ibat_current;
		lgcc_vote_fcc(LGCC_REASON_CTYPE, CHG_CURRENT_MAX);

		lge_power_changed(&cc->lge_cc_lpc);
	}

skip_ctype:
	if ((!cc->ctype_present && !cc->finish_check_ctype)
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
		&& !cc->is_hvdcp_present
#endif
		&& cc->is_usb_present)
		schedule_delayed_work(&cc->ctype_detect_work,
				CTYPE_CHECK_DELAY);
}
#endif

#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
#define HVDCP_IUSB_MAX 3000
#define HVDCP_IUSB_MIN 1800   /* Because of charging for ieee1725 at ocp part on datesheet */
#define HVDCP_IBAT_MIN 2000
#define HVDCP_SET_CUR_DELAY	msecs_to_jiffies(5 * 1000)
static void lge_hvdcp_set_ibat_work(struct work_struct *work){
	struct lge_charging_controller *cc =
		container_of(work, struct lge_charging_controller,
				hvdcp_set_cur_work.work);
	static int delay_counter;
	static int before_iusb;
	static int before_ibat;
	union power_supply_propval pval = {0, };
	int rc;
	bool taper_charging;
	union lge_power_propval lge_val = {0,};
	static int ibat_temp;
	static int first_time = true;
	int start_set_current = 0;

	pr_err("Check HVDCP\n");
	if (!cc->batt_psy)
		cc->batt_psy = power_supply_get_by_name("battery");
	if (cc->batt_psy) {
		rc = cc->batt_psy->get_property(cc->batt_psy,
				POWER_SUPPLY_PROP_CHARGE_TYPE, &pval);
		if (pval.intval == POWER_SUPPLY_CHARGE_TYPE_TAPER)
			taper_charging = true;
		else
			taper_charging = false;
	} else {
		taper_charging = false;
		goto skip_setting;
	}

	if (!cc->is_usb_present) {
		delay_counter = 0;
		ibat_temp = 0;
		start_set_current = 0;
		cc->ibat_current = 0;
		cc->finish_hvdcp_set_cur = 0;
		lgcc_vote_fcc(LGCC_REASON_HVDCP, -EINVAL);
		return;
	} else
		cc->ibat_current = HVDCP_IBAT_MIN;

	cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
			LGE_POWER_PROP_CHARGING_CURRENT_MAX, &lge_val);
	ibat_temp = lge_val.intval;
	pr_debug("ibat_temp %d\n", ibat_temp);

	if (delayed_work_pending(&cc->ctype_detect_work))
		cancel_delayed_work_sync(&cc->ctype_detect_work);

	if (!cc->finish_hvdcp_set_cur && !taper_charging) {
		delay_counter++;

		if (first_time) {
			if (lge_get_boot_mode() == LGE_BOOT_MODE_CHARGERLOGO) {
				if (delay_counter >= 4)
					start_set_current = 1;
			} else {
				if (delay_counter >= 8)
					start_set_current = 1;
				first_time = 0;
			}
		} else {
			if (delay_counter >= 3)
				start_set_current = 1;
		}
	}

	if (start_set_current) {
		cc->ibat_current = ibat_temp;
		if (cc->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
			cc->iusb_current = HVDCP_IUSB_MIN * 1000;
			cc->otp_ibat_current = cc->ibat_current;
			pr_info("QC3.0 ibat %d\n", cc->otp_ibat_current);
		} else if (cc->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
			cc->iusb_current = HVDCP_IUSB_MIN * 1000;
			cc->otp_ibat_current = cc->ibat_current;
			pr_info("QC2.0 ibat %d\n", cc->otp_ibat_current);

			cc->finish_hvdcp_set_cur = 1;
			delay_counter = 0;
			pr_err("Finish set hvdcp current\n");
		} else {
			cc->iusb_current = HVDCP_IUSB_MIN * 1000;
			cc->otp_ibat_current = cc->ibat_current * 1000;
		}
	} else {
		cc->iusb_current = HVDCP_IUSB_MIN * 1000;
		cc->otp_ibat_current = cc->ibat_current * 1000;
	}

	lgcc_vote_fcc(LGCC_REASON_HVDCP, cc->otp_ibat_current / 1000);

	if (before_iusb != cc->iusb_current ||
			before_ibat != cc->otp_ibat_current) {
		pr_info("Set_cur %d, iusb %d, ibat %d\n",
		cc->finish_hvdcp_set_cur,
		cc->iusb_current, cc->otp_ibat_current);
	}

	lge_power_changed(&cc->lge_cc_lpc);
	before_iusb = cc->iusb_current;
	before_ibat = cc->otp_ibat_current;

skip_setting:
	if (!cc->finish_hvdcp_set_cur && cc->is_usb_present
			&& !taper_charging && !cc->ctype_present)
		schedule_delayed_work(&cc->hvdcp_set_cur_work,
				HVDCP_SET_CUR_DELAY);
}
#endif

static void lge_cc_external_lge_power_changed(struct lge_power *lpc) {
	union lge_power_propval lge_val = {0,};
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	union power_supply_propval value = {0,};
#endif
	int rc = 0;
	static int before_usb_present;
	struct lge_charging_controller *cc
		= container_of(lpc, struct lge_charging_controller,
				lge_cc_lpc);
	int ibat = 0;
	int i;

	cc->lge_cd_lpc = lge_power_get_by_name("lge_cable_detect");
	if(!cc->lge_cd_lpc){
		pr_err("cable detection not found deferring probe\n");
	} else {
		rc = cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
				LGE_POWER_PROP_CHG_PRESENT, &lge_val);
		if (rc == 0) {
			cc->is_usb_present = lge_val.intval;

			if (before_usb_present != cc->is_usb_present) {
				pr_info("usb present : %d\n",lge_val.intval);
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
				if (!cc->batt_psy) {
					cc->batt_psy = power_supply_get_by_name("battery");
					if (!cc->batt_psy) {
						pr_err("batt_psy is not yet ready\n");
						value.intval = 0;
					}
				} else {
					cc->batt_psy->get_property(cc->batt_psy,
							POWER_SUPPLY_PROP_CAPACITY, &value);
				}
				lg_cc_stop_battemp_alarm(cc);

				if (cc->is_usb_present && (value.intval < 100)) {
					pr_info("start alarm\n");
					lg_cc_start_battemp_alarm(cc, BTM_ALARM_TIME(2));
				} else {
					pr_err("start work_queue\n");
					lg_cc_start_battemp_work(cc,2);
				}
#else

				lg_cc_stop_battemp_work(cc);
				lg_cc_start_battemp_work(cc,2);
#endif
				before_usb_present = lge_val.intval;
			}
			rc = cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
					LGE_POWER_PROP_TYPE, &lge_val);
			if (rc < 0) {
				pr_err("Failed to get type\n");
				cc->chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
			} else
				cc->chg_type = lge_val.intval;

			rc |= cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
					LGE_POWER_PROP_TA_CURRENT, &lge_val);
			if (rc < 0) {
				pr_err("Failed to get type\n");
				cc->ta_current = 0;
			} else
				cc->ta_current = lge_val.intval;

			rc |= cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
					LGE_POWER_PROP_USB_CURRENT, &lge_val);
			if (rc < 0) {
				pr_err("Failed to get type\n");
				cc->usb_current = 0;
			} else
				cc->usb_current = lge_val.intval;

			rc |= cc->lge_cd_lpc->get_property(cc->lge_cd_lpc,
					LGE_POWER_PROP_CHARGING_CURRENT_MAX, &lge_val);
			if (rc < 0) {
				pr_err("Failed to get type\n");
				ibat = 0;
			} else
				ibat = lge_val.intval;

			if (!cc->is_usb_present) {
				cc->ibat_current = 0;
				cc->finish_check_ctype = 0;
				schedule_delayed_work(&cc->ctype_detect_work, 0);
				schedule_delayed_work(&cc->hvdcp_set_cur_work, 0);
				for (i = 1; i < LGCC_REASON_MAX; i++) {
					lgcc_vote_fcc(i, -EINVAL);
				}
			} else {
				if (ibat <= 0)
					ibat = 500 * 1000;
			}

			pr_debug("type %d, ta %d, usb %d, ibat %d, present %d\n",
					cc->chg_type, cc->ta_current, cc->usb_current, ibat,
					cc->is_usb_present);

#ifdef CONFIG_LGE_USB_TYPE_C
			if (cc->chg_type == POWER_SUPPLY_TYPE_USB_DCP) {
				if (!cc->ctype_present && !cc->finish_check_ctype) {
					cc->ibat_current = CTYPE_IBAT_DEFAULT;
					if (!delayed_work_pending(&cc->ctype_detect_work))
						schedule_delayed_work(&cc->ctype_detect_work, 0);
				}
			}
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
			else if (cc->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP ||
					cc->chg_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
				cc->is_hvdcp_present = 1;
				if (!delayed_work_pending(&cc->hvdcp_set_cur_work) &&
						!cc->finish_hvdcp_set_cur)
					schedule_delayed_work(&cc->hvdcp_set_cur_work, 0);
			}
#endif
		}
	}
}

static int lge_charging_controller_probe(struct platform_device *pdev) {
	struct lge_charging_controller *cc;
	struct lge_power *lge_power_cc;
	int ret;

	cc = kzalloc(sizeof(struct lge_charging_controller),
								GFP_KERNEL);
	if (!cc) {
		pr_err("lge_charging_controller memory alloc failed.\n");
		return -ENOMEM;
	}

	cc->dev = &pdev->dev;
	the_cc = cc;
	cc->chg_enable = -1;

	platform_set_drvdata(pdev, cc);

	wake_lock_init(&cc->lcs_wake_lock,
			WAKE_LOCK_SUSPEND, "lge_charging_scenario");
#ifndef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	wake_lock_init(&cc->chg_wake_lock,
			WAKE_LOCK_SUSPEND, "lge_charging_wake_lock");
#endif
	INIT_DELAYED_WORK(&cc->battemp_work,
			lge_monitor_batt_temp_work);
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	alarm_init(&cc->btm_polling_alarm, ALARM_REALTIME,
			lgcc_btm_alarm);
	wake_lock_init(&cc->alarm_wake_lock,
			WAKE_LOCK_SUSPEND, "lge_charging_scenario_alarm");
#endif
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	INIT_DELAYED_WORK(&cc->hvdcp_set_cur_work, lge_hvdcp_set_ibat_work);
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	INIT_DELAYED_WORK(&cc->ctype_detect_work, lge_check_typec_work);
#endif
	lge_power_cc = &cc->lge_cc_lpc;
	lge_power_cc->name = "lge_cc";
	lge_power_cc->properties = lge_power_lge_cc_properties;
	lge_power_cc->num_properties =
		ARRAY_SIZE(lge_power_lge_cc_properties);
	lge_power_cc->get_property = lge_power_lge_cc_get_property;
	lge_power_cc->set_property = lge_power_lge_cc_set_property;
	lge_power_cc->property_is_writeable =
		lge_power_lge_cc_property_is_writeable;
	lge_power_cc->supplied_to = lge_cc_supplied_to;
	lge_power_cc->num_supplicants = ARRAY_SIZE(lge_cc_supplied_to);
	lge_power_cc->lge_supplied_from = lge_cc_supplied_from;
	lge_power_cc->num_lge_supplies	= ARRAY_SIZE(lge_cc_supplied_from);
	lge_power_cc->external_lge_power_changed
			= lge_cc_external_lge_power_changed;

	ret = lge_power_register(cc->dev, lge_power_cc);
	if (ret < 0) {
		pr_err("Failed to register lge power class: %d\n", ret);
		goto err_free;
	}

	cc->chg_current_max = -EINVAL;
	cc->chg_current_te = cc->chg_current_max;

	cc->otp_ibat_current = -1;

	cc->start_batt_temp = 5;
	lg_cc_start_battemp_work(cc, cc->start_batt_temp);
	cc->test_chg_scenario = 0;
	cc->test_batt_therm_value = 25;
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_TYPE_HVDCP
	cc->is_hvdcp_present = 0;
	cc->update_hvdcp_state = 0;
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	cc->update_ctype_state = 0;
#endif

	pr_info("LG Charging controller probe done~!!\n");

	return 0;

err_free:
	kfree(cc);
	return ret;
}

#ifdef CONFIG_OF
static struct of_device_id lge_charging_controller_match_table[] = {
	{.compatible = "lge,charging_controller"},
	{ },
};
#endif

static int lge_charging_controller_remove(struct platform_device *pdev) {
	lge_power_unregister(&the_cc->lge_cc_lpc);
#ifdef CONFIG_LGE_PM_LGE_POWER_CLASS_CHARGER_SLEEP
	alarm_cancel(&the_cc->btm_polling_alarm);
#endif
	kfree(the_cc);
	return 0;
}

static struct platform_driver lge_charging_controller_driver = {
	.probe = lge_charging_controller_probe,
	.remove = lge_charging_controller_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = lge_charging_controller_match_table,
#endif
	},
};

static int __init lge_charging_controller_init(void) {
	return platform_driver_register(&lge_charging_controller_driver);
}

static void __exit lge_charging_controller_exit(void) {
	platform_driver_unregister(&lge_charging_controller_driver);
}

module_init(lge_charging_controller_init);
module_exit(lge_charging_controller_exit);

MODULE_DESCRIPTION("LGE Charging Controller driver");
MODULE_LICENSE("GPL v2");
