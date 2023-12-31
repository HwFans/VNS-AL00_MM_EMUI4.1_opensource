/************************************************************
*
* Copyright (C), 1988-2015, Huawei Tech. Co., Ltd.
* FileName: huawei_dsm_charger.c
* Author: jiangfei(00270021)       Version : 0.1      Date:  2015-03-17
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
*  Description:    .c adatper file for charger radar
*  Version:
*  Function List:
*  History:
*  <author>  <time>   <version >   <desc>
***********************************************************/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <asm/irq.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/syscalls.h>
#include <linux/power_supply.h>
#include <linux/rtc.h>
#include <linux/power/huawei_dsm_charger.h>
#include <linux/power/huawei_charger.h>

struct hw_dsm_charger_info
{
    struct device        *dev;
    struct delayed_work   check_charging_batt_status_work;
    struct power_supply *batt_psy;
    struct power_supply *usb_psy;
    struct mutex		dsm_dump_lock;
    struct hw_batt_temp_info *temp_ctrl;
    struct dsm_charger_ops *chg_ops;
    struct dsm_bms_ops *bms_ops;
    struct dsm_err_info *dsm_err;
    bool    charging_disabled;
    int    error_no;
};

struct hw_dsm_charger_info *dsm_info = NULL;

/*bms dsm client definition */
struct dsm_dev dsm_bms =
{
    .name = "dsm_bms", // dsm client name
    .fops = NULL,
    .buff_size = 4096, // buffer size
};
struct dsm_client *bms_dclient = NULL;
EXPORT_SYMBOL(bms_dclient);

/*charger dsm client definition */
struct dsm_dev dsm_charger =
{
    .name = "dsm_charger", // dsm client name
    .fops = NULL,
    .buff_size = 4096, // buffer size
};
struct dsm_client *charger_dclient = NULL;
EXPORT_SYMBOL(charger_dclient);

struct hw_batt_temp_info temp_info =
{
    .cold_bat_degree = 0, //default cold bat degree: 0 degree
    .cool_bat_degree = 100, //default cool bat degree: 10 degree
    .imaxma_cool_bat = 800, //default cool bat charge current: 600mA
    .vmaxmv_cool_bat = 4400, //default cool bat max voltage: 4350mV
    .warm_bat_degree = 450, //default warm bat degree: 42 degree
    .imaxma_warm_bat = 950, //default warm bat charge current: 600mA
    .vmaxmv_warm_bat = 4100, //default warm bat max voltage: 4100mV
    .hot_bat_degree = 550, //default hot bat degree: 52 degree
};

/* radar error count struct, same error report 3 time*/
struct dsm_err_info dsm_err;

static int temp_resume_flag = 0;
SqQueue chg_bms_info = {.head = -1, .tail = -1};

extern bool uvlo_event_trigger;
static int dump_bms_chg_info(struct hw_dsm_charger_info *di, struct dsm_client *dclient,int type, char *info)
{
    if(NULL == dclient)
    {
        pr_err("%s: there is no dclient!\n", __func__);
        return -1;
    }
    di->dsm_err->err_no = type - PMU_ERR_NO_MIN;
    if(di->dsm_err->count[di->dsm_err->err_no]++ < REPORT_MAX)
    {
        mutex_lock(&di->dsm_dump_lock);

        /* try to get permission to use the buffer */
        if(dsm_client_ocuppy(dclient))
        {
            /* buffer is busy */
            pr_err("%s: buffer is busy!\n", __func__);
            mutex_unlock(&di->dsm_dump_lock);
            return -1;
        }
        dsm_client_record(dclient, "%s\n", info);
        dsm_client_notify(dclient, type);

        mutex_unlock(&di->dsm_dump_lock);
        return 0;
    }
    else
    {
        di->dsm_err->count[di->dsm_err->err_no] = REPORT_MAX;
    }
    return 0;
}

int dsm_post_chg_bms_info(int erro_no, char *pstr)
{
    int *qhead = NULL, *qtail = NULL;

    if ((erro_no < PMU_ERR_NO_MIN) || (erro_no > PMU_ERR_NO_MAX))
    {
        pr_err("%s: erro_no:%d is out of range!\n", __func__, erro_no);
        return -1;
    }
    qhead = &chg_bms_info.head;
    qtail = &chg_bms_info.tail;
    if (-1 == *qtail)
    {
        *qtail = 0;
    }
    else if ((*qtail+1)%QUEUE_INIT_SIZE == *qhead)
    {
        /*queue full*/
        pr_err("%s: queue is already full!\n", __func__);
        return -1;
    }

    memset(chg_bms_info.base[*qtail].content_info, 0, sizeof(chg_bms_info.base[*qtail].content_info));
    if (pstr)
    {
        strncpy(chg_bms_info.base[*qtail].content_info, pstr, sizeof(chg_bms_info.base[*qtail].content_info)-1);
    }
    chg_bms_info.base[*qtail].erro_number = erro_no;
    *qtail = (*qtail + 1)%QUEUE_INIT_SIZE;
    if (*qhead == -1)
    {
        *qhead = 0;
    }
    pr_debug("%s: queue add success!\n", __func__);
    return 0;
}

static int dsm_get_chg_bms_info(void)
{
    int qhead = 0, qtail = 0;
    struct hw_dsm_charger_info *di = NULL;
    di = dsm_info;

    if(NULL == di)
    {
        /*dclient is not ready*/
        pr_err("%s: dclient is not ready!\n", __func__);
        return -1;
    }
    pr_debug("%s: dclient is ready!\n", __func__);
    qhead = chg_bms_info.head;
    qtail = chg_bms_info.tail;
    if (qhead == qtail)
    {
        /*queue empty*/
        pr_debug( "%s: queue is empty!\n",__func__);
        return 0;
    }
    else
    {
        chg_bms_info.head = (chg_bms_info.head + 1)%QUEUE_INIT_SIZE;
        if ((chg_bms_info.base[qhead].erro_number >= DSM_BMS_NORMAL_SOC_CHANGE_MUCH) && (chg_bms_info.base[qhead].erro_number < DSM_NOT_CHARGE_WHEN_ALLOWED))
        {
            dump_bms_chg_info(di,bms_dclient,chg_bms_info.base[qhead].erro_number,chg_bms_info.base[qhead].content_info);
        }
        else if ((chg_bms_info.base[qhead].erro_number >= DSM_NOT_CHARGE_WHEN_ALLOWED) && (chg_bms_info.base[qhead].erro_number < DSM_ABNORMAL_POWERON_REASON_1))
        {
            dump_bms_chg_info(di,charger_dclient,chg_bms_info.base[qhead].erro_number,chg_bms_info.base[qhead].content_info);
        }
        else
        {
            pr_err("%s: err_no is exceed available number, do nothing!\n", __func__);
            return -1;
        }

        return 0;
    }
}

static int is_battery_in_normal_condition(int voltage ,int temp,struct hw_dsm_charger_info *chip)
{
    if((temp >= (chip->temp_ctrl->warm_bat_degree - TEMP_BUFFER))
       && (temp < (chip->temp_ctrl->hot_bat_degree - TEMP_BUFFER))
       && (voltage <= (chip->temp_ctrl->vmaxmv_warm_bat - WARM_VOL_BUFFER)))
    {
            return 1;
    }
    else if(((chip->temp_ctrl->cold_bat_degree + TEMP_BUFFER) < temp)
              && (temp < (chip->temp_ctrl->warm_bat_degree - TEMP_BUFFER)))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int is_batt_temp_in_normal_range(int pre_temp,int curr_temp)
{
    if((TEMP_LOWER_THR <=curr_temp) && (TEMP_UPPER_THR >= curr_temp)
        && (TEMP_LOWER_THR <= pre_temp) && (TEMP_UPPER_THR >= pre_temp)
        && (INIT_TEMP != pre_temp))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static void print_basic_info_before_dump(struct dsm_client *dclient, const int type)
{
    int error_type;

    error_type = type;
    switch(error_type)
    {
    case DSM_BMS_VOL_SOC_DISMATCH_1:
        dsm_client_record(dclient,
                  "battery voltage is over 3.6V, but soc is "
                  "0 percent, not match\n");
        pr_info("battery voltage is over 3.6V, but soc is "
                "0 percent, not match\n");
        break;

    case DSM_BMS_VOL_SOC_DISMATCH_2:
        dsm_client_record(dclient,
                  "battery voltage is over 3.7V, but soc is "
                  "no more than 2 percent, not match\n");
        pr_info("battery voltage is over 3.7V, but soc is "
                "no more than 2 persent, not match\n");
        break;

    case DSM_BMS_VOL_SOC_DISMATCH_3:
        dsm_client_record(dclient,
                  "battery voltage is over 4.35V, but  "
                  "soc is below 90 percent\n");
        pr_info("battery voltage is over 4.35V, but  "
                "soc is below 90 percent\n");
        break;

    case DSM_VM_BMS_VOL_SOC_DISMATCH_4:
        dsm_client_record(dclient,
                  "battery voltage is too low when "
                  "soc is 2 percent\n");
        pr_info("battery voltage is too low when "
                "soc is 2 percent\n");
        break;

    case DSM_NOT_CHARGE_WHEN_ALLOWED:
        dsm_client_record(dclient,
                  "cannot charging when allowed charging\n");
        pr_info("cannot charging when allowed charging\n");
        break;

    case DSM_BATT_PRES_ERROR_NO:
        dsm_client_record(dclient,
                  "battery is absent!\n");
        pr_info("battery is absent!\n");
        break;

    case DSM_WARM_CURRENT_LIMIT_FAIL:
        dsm_client_record(dclient,
                  "set battery warm charge current failed\n");
        pr_info("set battery warm charge current failed\n");
        break;

    case DSM_COOL_CURRENT_LIMIT_FAIL:
        dsm_client_record(dclient,
                   "set battery cool charge current failed\n");
        pr_info("set battery cool charge current failed\n");
        break;

    case DSM_FULL_WHEN_CHARGER_ABSENT:
        dsm_client_record(dclient,
                  "battery status is full when charger is absent\n");
        pr_info("battery status is full when charger is absent\n");
        break;

    case DSM_BATT_VOL_OVER_4450:
        dsm_client_record(dclient,
                  "battery voltage is over 4.45V\n");
        pr_info("battery voltage is over 4.45V\n");
        break;

    case DSM_FAKE_FULL:
        dsm_client_record(dclient,
                   "report charging full when actual soc is below 95 percent\n");
        pr_info("report charging full when actual soc is below 95 percent\n");
        break;

    case DSM_ABNORMAL_CHARGE_STATUS:
        dsm_client_record(dclient,
                  "charging status is charging while charger is not online\n");
        pr_info("charging status is charging while charger is not online\n");
        break;

    case DSM_BATT_VOL_TOO_LOW:
        dsm_client_record(dclient,
                   "battery voltage is too low(below 2.5V)\n");
        pr_info("battery voltage is too low(below 2.5V)\n");
        break;

    case DSM_STILL_CHARGE_WHEN_HOT:
        dsm_client_record(dclient,
                   "still charge when battery is hot\n");
        pr_info("still charge when battery is hot\n");
        break;

    case DSM_STILL_CHARGE_WHEN_COLD:
        dsm_client_record(dclient,
                   "still charge when battery is cold\n");
        pr_info("still charge when battery is cold\n");
        break;

    case DSM_STILL_CHARGE_WHEN_SET_DISCHARGE:
        dsm_client_record(dclient,
                   "still charge when we set discharge\n");
        pr_info("still charge when we set discharge\n");
        break;

    case DSM_STILL_CHARGE_WHEN_VOL_OVER_4450:
        dsm_client_record(dclient,
                   "still charge when battery voltage reach or over 4.45V\n");
        pr_info("still charge when battery voltage reach or over 4.45V\n");
        break;

    case DSM_HEATH_OVERHEAT:
        dsm_client_record(dclient,
                   "battery health is overheat\n");
        pr_info("battery health is overheat\n");
        break;

    case DSM_BATT_TEMP_JUMP:
        dsm_client_record(dclient,
                   "battery temperature change more than 5 degree in short time\n");
        pr_info("battery temperature change more than 5 degree in short time\n");
        break;

    case DSM_BATT_TEMP_BELOW_0:
        dsm_client_record(dclient,
                   "battery temperature is below 0 degree\n");
        pr_info("battery temperature is below 0 degree\n");
        break;

    case DSM_BATT_TEMP_OVER_60:
        dsm_client_record(dclient,
                   "battery temperature is over 60 degree\n");
        pr_info("battery temperature is over 60 degree\n");
        break;

    case DSM_NOT_CHARGING_WHEN_HOT:
        dsm_client_record(dclient,
                   "battery is hot, not charging\n");
        pr_info("battery is hot, not charging\n");
        break;

    case DSM_NOT_CHARGING_WHEN_COLD:
        dsm_client_record(dclient,
                   "battery is cold, not charging\n");
        pr_info("battery is cold, not charging\n");
        break;

    case DSM_ABNORMAL_CHARGE_FULL_STATUS:
        dsm_client_record(dclient,
                   "report discharging but actual soc is 100 percent\n");
        pr_info("report discharging but actual soc is 100 percent\n");
        break;

    default:
        break;
    }
}

/* dump charger ic, bms registers and some adc values, and notify to dsm*/
static int dump_info_and_adc(struct hw_dsm_charger_info *di, struct dsm_client *dclient, int type)
{
    int vbat_uv = 0, batt_temp = 250, current_ma = 0;
    union power_supply_propval val = {0,};

    if( NULL == dclient )
    {
        pr_err("%s: there is no dclient!\n", __func__);
        return -1;
    }

    mutex_lock(&di->dsm_dump_lock);

    /* try to get permission to use the buffer */
    if(dsm_client_ocuppy(dclient))
    {
        /* buffer is busy */
        pr_err("%s: buffer is busy!\n", __func__);
        mutex_unlock(&di->dsm_dump_lock);
        return -1;
    }

    print_basic_info_before_dump(dclient, type);

    /*save battery vbat, current, temp values and so on*/
    di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_TEMP,&val);
    batt_temp = val.intval;
    di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_VOLTAGE_NOW,&val);
    vbat_uv = val.intval;
    di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CURRENT_NOW,&val);
    current_ma = val.intval;

    dsm_client_record(dclient,
                      "ADC values: vbat=%d current=%d batt_temp=%d\n",
                      vbat_uv, current_ma, batt_temp);

    pr_info("ADC values: vbat=%d current=%d batt_temp=%d\n",
            vbat_uv, current_ma, batt_temp);

    dsm_client_notify(dclient, type);
    mutex_unlock(&di->dsm_dump_lock);
    return 0;
}

/* interface for be called to dump and notify*/
int dsm_dump_log(struct dsm_client *dclient, int err_no)
{
    struct hw_dsm_charger_info *di = NULL;

    di = dsm_info;
    if( NULL == di )
    {
        pr_err("%s: dsm_charger is not ready!\n", __func__);
        return -1;
    }
    if((PMU_ERR_NO_MIN > err_no) 
    || (PMU_ERR_NO_MAX < err_no))
    {
        pr_err("%s: err_no is exceed available number, do nothing!\n", __func__);
        return -1;
    }
    di->dsm_err->err_no = err_no - DSM_BMS_NORMAL_SOC_CHANGE_MUCH;
    if(di->dsm_err->count[di->dsm_err->err_no]++ < REPORT_MAX)
    {
        dump_info_and_adc(di, dclient, err_no);
    }
    else
    {
        di->dsm_err->count[di->dsm_err->err_no] = REPORT_MAX;
    }
    return 0;
}
EXPORT_SYMBOL(dsm_dump_log);

static int get_current_time(unsigned long *now_tm_sec)
{
    struct rtc_time tm;
    struct rtc_device *rtc;
    int rc;

    rtc = rtc_class_open(CONFIG_RTC_HCTOSYS_DEVICE);
    if (rtc == NULL)
    {
        pr_err("%s: unable to open rtc device (%s)\n",
               __FILE__, CONFIG_RTC_HCTOSYS_DEVICE);
        return -EINVAL;
    }

    rc = rtc_read_time(rtc, &tm);
    if (rc)
    {
        pr_err("Error reading rtc device (%s) : %d\n",
               CONFIG_RTC_HCTOSYS_DEVICE, rc);
        goto close_time;
    }

    rc = rtc_valid_tm(&tm);
    if (rc)
    {
        pr_err("Invalid RTC time (%s): %d\n",
               CONFIG_RTC_HCTOSYS_DEVICE, rc);
        goto close_time;
    }
    rtc_tm_to_time(&tm, now_tm_sec);

close_time:
    rtc_class_close(rtc);
    return rc;
}


static void check_charging_batt_status_work(struct work_struct *work)
{
    int vbat_uv, batt_temp, bat_present, cur_status;
    int health, batt_level, usb_present, input_current=0;
    static int cannot_charge_count = 0;
    static int start_dismatch_detect = 0;
    static int hot_charging_count = 0, cold_charging_count = 0;
    static int warm_exceed_limit_count = 0,  cool_exceed_limit_count = 0;
    static int previous_temp = INIT_TEMP;
    int current_max = 0, current_ma = 0;
    int voltage_regulation = 0;
    static unsigned long previous_tm_sec = 0;
    unsigned long now_tm_sec = 0;
    union power_supply_propval val = {0,};
    struct hw_dsm_charger_info *chip =
        container_of(work, struct hw_dsm_charger_info, check_charging_batt_status_work.work);

    usb_present = is_usb_chg_exist();

    if(chip->batt_psy && chip->batt_psy->get_property)
    {
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_CAPACITY,&val);
        batt_level = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_PRESENT,&val);
        bat_present = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_TEMP,&val);
        batt_temp = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_VOLTAGE_NOW,&val);
        vbat_uv = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_CURRENT_NOW,&val);
        current_ma = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_HEALTH,&val);
        health = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_CHARGING_ENABLED,&val);
        chip->charging_disabled = !(val.intval);
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_STATUS,&val);
        cur_status = val.intval;
        chip->batt_psy->get_property(chip->batt_psy,POWER_SUPPLY_PROP_INPUT_CURRENT_MAX,&val);
        input_current = val.intval;
    }

    chip->usb_psy->get_property(chip->usb_psy,
                                POWER_SUPPLY_PROP_CURRENT_MAX, &val);
    current_max = val.intval / 1000;
	
    /*if all the charging conditions are avaliable, but it still cannot charge, */
    /*we report the error and dump the basic info and ADC values  */
    if(is_battery_in_normal_condition(vbat_uv,batt_temp,chip))
    {

        if((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
           && (BATT_FULL_LEVEL != batt_level) && usb_present
           && (current_max > 2)
           && (!chip->charging_disabled) && bat_present && (POWER_SUPPLY_STATUS_FULL != cur_status))
        {

            if(cannot_charge_count++ < NOT_CHARGE_COUNT)
            {
                pr_info("cannot charge when allowed, count is %d\n", cannot_charge_count);
            }
            else
            {
                cannot_charge_count = 0;
                pr_info("cannot charge when allowed!\n");
                dsm_dump_log(charger_dclient, DSM_NOT_CHARGE_WHEN_ALLOWED);
            }
        }
        else
        {
            cannot_charge_count = 0;
        }
    }

    if(!bat_present)  /* battery absent */
    {
        pr_info("battery is absent!\n");
        dsm_dump_log(charger_dclient, DSM_BATT_PRES_ERROR_NO);
    }
    else
    {
        if(!usb_present)  /* usb absent, discharging */
        {
            if(START_DISMATCH_COUNT <= start_dismatch_detect++)
            {
                start_dismatch_detect = START_DISMATCH_COUNT;
                if((VOL_THR1 <= vbat_uv) && (SOC_ZERO == batt_level))
                {
                    dsm_dump_log(charger_dclient, DSM_BMS_VOL_SOC_DISMATCH_1);
                }

                if((VOL_THR2 <= vbat_uv) && (SOC_ZERO != batt_level)
                        && (SOC_THR1 >= batt_level))
                {
                    dsm_dump_log(charger_dclient, DSM_BMS_VOL_SOC_DISMATCH_2);
                }

                if((VOL_HIGH <= vbat_uv) && (SOC_ZERO != batt_level)
                        && (SOC_HIGH >= batt_level))
                {
                    dsm_dump_log(charger_dclient, DSM_BMS_VOL_SOC_DISMATCH_3);
                }

                if((VOL_TOO_LOW >= vbat_uv) && (SOC_THR1 == batt_level))
                {
                    dsm_dump_log(charger_dclient, DSM_VM_BMS_VOL_SOC_DISMATCH_4);
                }
            }

            if(HIGH_VOL <= vbat_uv)
            {
                dsm_dump_log(charger_dclient, DSM_BATT_VOL_OVER_4450);
            }

            if(POWER_SUPPLY_STATUS_CHARGING == cur_status)
            {
                dsm_dump_log(charger_dclient, DSM_ABNORMAL_CHARGE_STATUS);
            }

            if(POWER_SUPPLY_STATUS_FULL == cur_status)
            {
                dsm_dump_log(charger_dclient, DSM_FULL_WHEN_CHARGER_ABSENT);
            }
        }

        /* usb present */
        if(usb_present)
        {
            if((POWER_SUPPLY_STATUS_FULL == cur_status)
                    && (SOC_HIGH_THR >= batt_level))
            {
                dsm_dump_log(charger_dclient, DSM_FAKE_FULL);
            }
            if((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
                    && (BATT_FULL_LEVEL == batt_level))
            {
                dsm_dump_log(charger_dclient, DSM_ABNORMAL_CHARGE_FULL_STATUS);
            }
            /* chip->charging_disabled is true*/
            if(chip->charging_disabled)
            {
                if(POWER_SUPPLY_STATUS_CHARGING == cur_status)
                {
                    dsm_dump_log(charger_dclient, DSM_STILL_CHARGE_WHEN_SET_DISCHARGE);
                }
            }
            else
            {
                voltage_regulation = VOL_REGULATION_MAX;
                if((voltage_regulation <= vbat_uv)&&(POWER_SUPPLY_STATUS_CHARGING == cur_status))
                {
                    dsm_dump_log(charger_dclient, DSM_STILL_CHARGE_WHEN_VOL_OVER_4450);
                }

                if(((chip->temp_ctrl->hot_bat_degree + TEMP_BUFFER) < batt_temp)
                        &&((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
                           ||(POWER_SUPPLY_STATUS_NOT_CHARGING == cur_status)))
                {
                    dsm_dump_log(charger_dclient, DSM_NOT_CHARGING_WHEN_HOT);
                }

                if(((chip->temp_ctrl->cold_bat_degree - TEMP_BUFFER) > batt_temp)
                        &&((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
                           ||(POWER_SUPPLY_STATUS_NOT_CHARGING == cur_status)))
                {
                    dsm_dump_log(charger_dclient, DSM_NOT_CHARGING_WHEN_COLD);
                }

                if(((chip->temp_ctrl->hot_bat_degree + TEMP_BUFFER) < batt_temp)
                        &&(POWER_SUPPLY_STATUS_CHARGING == cur_status))
                {
                    if(hot_charging_count++ < DSM_COUNT)
                    {
                        pr_info("still charge when battery is hot, count is %d\n", hot_charging_count);
                    }
                    else
                    {
                        hot_charging_count = 0;
                        dsm_dump_log(charger_dclient, DSM_STILL_CHARGE_WHEN_HOT);
                    }
                }
                else
                {
                    hot_charging_count = 0;
                }

                if(((chip->temp_ctrl->cold_bat_degree - TEMP_BUFFER) > batt_temp)
                        &&(POWER_SUPPLY_STATUS_CHARGING == cur_status))
                {
                    if(cold_charging_count++ < DSM_COUNT)
                    {
                        pr_info("still charge when battery is cold, count is %d\n", cold_charging_count);
                    }
                    else
                    {
                        cold_charging_count = 0;
                        dsm_dump_log(charger_dclient, DSM_STILL_CHARGE_WHEN_COLD);
                    }
                }
                else
                {
                    cold_charging_count = 0;
                }

                if(((chip->temp_ctrl->warm_bat_degree + TEMP_BUFFER) < batt_temp)
                        &&(WARM_COOL_CURRENT_LIMIT < input_current))
                {
                    if(warm_exceed_limit_count++ < DSM_COUNT)
                    {
                        pr_info("current is over warm current limit when warm, count is %d\n", warm_exceed_limit_count);
                    }
                    else
                    {
                        warm_exceed_limit_count = 0;
                        dsm_dump_log(charger_dclient, DSM_WARM_CURRENT_LIMIT_FAIL);
                    }
                }
                else
                {
                    warm_exceed_limit_count = 0;
                }

                if(((chip->temp_ctrl->cool_bat_degree - TEMP_BUFFER) > batt_temp)
                        &&(WARM_COOL_CURRENT_LIMIT < input_current))
                {
                    if(cool_exceed_limit_count++ < DSM_COUNT)
                    {
                        pr_info("current is over cool current limit when cool, count is %d\n", cool_exceed_limit_count);
                    }
                    else
                    {
                        cool_exceed_limit_count = 0;
                        dsm_dump_log(charger_dclient, DSM_COOL_CURRENT_LIMIT_FAIL);
                    }
                }
                else
                {
                    cool_exceed_limit_count = 0;
                }
            }
        }

        /* only care 20 to 40 temperature zone in centigrade, */
        /* if temp jumps in this zone in 30 seconds, notify to dsm server*/
        get_current_time(&now_tm_sec);
        if((abs(previous_temp - batt_temp) >= TEMP_DELTA)
            && is_batt_temp_in_normal_range(previous_temp,batt_temp)
            && (HALF_MINUTE >=(now_tm_sec -previous_tm_sec)))
        {
            if(temp_resume_flag)
            {
                pr_debug("%s:  temp_resume_flag is 1, ignore battery temp jump once\n", __func__);
                temp_resume_flag = 0;
            }
            else
            {
                dsm_dump_log(charger_dclient, DSM_BATT_TEMP_JUMP);
            }
        }
        previous_temp = batt_temp;
        previous_tm_sec = now_tm_sec;

        if(LOW_VOL >= vbat_uv)
        {
            dsm_dump_log(charger_dclient, DSM_BATT_VOL_TOO_LOW);
        }

        if(POWER_SUPPLY_HEALTH_OVERHEAT == health)
        {
            dsm_dump_log(charger_dclient, DSM_HEATH_OVERHEAT);
        }

        if(HOT_TEMP_60 < batt_temp)
        {
            dsm_dump_log(charger_dclient, DSM_BATT_TEMP_OVER_60);
        }

        if(LOW_TEMP > batt_temp)
        {
            dsm_dump_log(charger_dclient, DSM_BATT_TEMP_BELOW_0);
        }

        if(uvlo_event_trigger)
        {
            if(ABNORMAL_UVLO_VOL_THR <= vbat_uv)
            {
                dsm_dump_log(bms_dclient, DSM_BMS_HIGH_VOLTAGE_UVLO);
            }
            uvlo_event_trigger = false;
        }
    }
    dsm_get_chg_bms_info();

    schedule_delayed_work(&chip->check_charging_batt_status_work,
                          msecs_to_jiffies(CHECKING_TIME));
}

static int huawei_dsm_charger_probe(struct platform_device *pdev)
{
    struct hw_dsm_charger_info *chip;
    struct power_supply *usb_psy;
    struct power_supply *batt_psy;

    usb_psy = power_supply_get_by_name("usb");
    if (!usb_psy)
    {
        pr_err("usb supply not found deferring probe\n");
        return -EPROBE_DEFER;
    }
    batt_psy = power_supply_get_by_name("battery");
    if (!batt_psy)
    {
        pr_err("batt supply not found deferring probe\n");
        return -EPROBE_DEFER;
    }

    pr_info("%s: ------entry.\n", __func__);

    chip = kzalloc(sizeof(*chip), GFP_KERNEL);
    if (!chip)
    {
        pr_err("alloc mem failed!\n");
        return -ENOMEM;
    }
    chip->usb_psy = usb_psy;
    chip->batt_psy = batt_psy;
    chip->dev = &pdev->dev;
    dev_set_drvdata(chip->dev, chip);

    chip->temp_ctrl = &temp_info;
    chip->dsm_err = &dsm_err;
    mutex_init(&chip->dsm_dump_lock);

    dsm_info = chip;

    if (!bms_dclient)
    {
        bms_dclient = dsm_register_client(&dsm_bms);
    }

    if (!charger_dclient)
    {
        charger_dclient = dsm_register_client(&dsm_charger);
    }

    INIT_DELAYED_WORK(&chip->check_charging_batt_status_work,
                      check_charging_batt_status_work);

    schedule_delayed_work(&chip->check_charging_batt_status_work,
                          msecs_to_jiffies(DELAY_TIME));

    pr_info("%s: ------OK.\n", __func__);
    return 0;
}

static int  huawei_dsm_charger_remove(struct platform_device *pdev)
{
    struct hw_dsm_charger_info *chip = dev_get_drvdata(&pdev->dev);
    cancel_delayed_work_sync(&chip->check_charging_batt_status_work);
    mutex_destroy(&chip->dsm_dump_lock);
    return 0;
}

static int dsm_charger_suspend(struct device *dev)
{
    struct hw_dsm_charger_info *chip = dev_get_drvdata(dev);

    cancel_delayed_work_sync(&chip->check_charging_batt_status_work);

    return 0;
}
static int dsm_charger_resume(struct device *dev)
{
    struct hw_dsm_charger_info *chip = dev_get_drvdata(dev);

    temp_resume_flag = 1;
    schedule_delayed_work(&chip->check_charging_batt_status_work,
                          msecs_to_jiffies(0));

    return 0;
}

static const struct dev_pm_ops hw_dsm_pm_ops =
{
    .suspend	= dsm_charger_suspend,
    .resume	=dsm_charger_resume,
};


static struct of_device_id platform_hw_charger_ids[] =
{
    {
        .compatible = "huawei,dsm_charger",
        .data = NULL,
    },
    {
    },
};

static struct platform_driver huawei_dsm_charger_driver =
{
    .probe        = huawei_dsm_charger_probe,
    .remove        = huawei_dsm_charger_remove,
    .driver        = {
        .name    = "dsm_charger",
        .owner    = THIS_MODULE,
        .of_match_table = of_match_ptr(platform_hw_charger_ids),
        .pm		= &hw_dsm_pm_ops,
    },
};

static int __init huawei_dsm_charger_init(void)
{
    int ret = 0;
    ret = platform_driver_register(&huawei_dsm_charger_driver);

    return ret;
}
late_initcall_sync(huawei_dsm_charger_init);

static void __exit huawei_dsm_charger_exit(void)
{
    platform_driver_unregister(&huawei_dsm_charger_driver);
}

module_exit(huawei_dsm_charger_exit);

MODULE_AUTHOR("jiangfei<fei.jiang@huawei.com>");
MODULE_DESCRIPTION("hw dsm charger driver");
MODULE_LICENSE("GPL");
