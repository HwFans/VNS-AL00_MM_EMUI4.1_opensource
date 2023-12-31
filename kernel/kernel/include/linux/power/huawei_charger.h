/*
 * drivers/power/huawei_charger.h
 *
 *huawei charger driver
 *
 * Copyright (C) 2012-2015 HUAWEI, Inc.
 * Author: HUAWEI, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/qpnp/qpnp-adc.h>

#ifndef _HUAWEI_CHARGER
#define _HUAWEI_CHARGER

/*************************marco define area***************************/
#ifndef TRUE
#define TRUE (1)
#endif
#ifndef FALSE
#define FALSE (0)
#endif

/*Running test result*/
/*And charge abnormal info*/
#define CHARGE_STATUS_FAIL  (0<<0) //Indicate running test charging status fail
#define CHARGE_STATUS_PASS  (1<<0) //Indicate running test charging status pass
#define BATTERY_FULL            (1<<1)
#define USB_NOT_PRESENT         (1<<2)
#define REGULATOR_BOOST        (1<<3)
#define CHARGE_LIMIT           (1<<4)
#define BATTERY_HEALTH      (1<<5)
#define CHARGER_OVP         (1<<6)
#define OCP_ABNORML         (1<<7)
#define BATTERY_VOL_ABNORML     (1<<8)
#define BATTERY_TEMP_ABNORML    (1<<9)
#define BATTERY_ABSENT          (1<<10)

#define CHARGE_OCP_THR    -4500000 //charge current abnormal threshold
#define BATTERY_OCP_THR    6000000 //discharge current abnormal threshold
#define BATTERY_VOL_THR_HI  4500000 //battery voltage abnormal high threshold
#define BATTERY_VOL_THR_LO  2500000 //battery voltage abnormal low threshold
#define BATTERY_TEMP_HI  780 //battery high temp threshold
#define BATTERY_TEMP_LO  -100 //battery low temp threshold
#define WARM_VOL_BUFFER  100 //warm_bat_mv need have a 100mV buffer
#define WARM_TEMP_THR       390 //battery warm temp threshold for running test
#define HOT_TEMP_THR        600 //battery overheat temp threshold for running test
#define BATT_FULL           100 //battery full capactiy
#define COLD_HOT_TEMP_BUFFER        200 //temp buffer
#define PASS_MASK           0x1E    //Running test pass mask
#define FAIL_MASK           0x7E0   //Running test fail mask
#define WARM_VOL_THR        4100 //battery regulation voltage in mV when warm
#define HOT_TEMP        650 //battery hot temp threshold
#define COLD_TEMP       0 //battery cold temp threshold

#define MAX_SIZE    2048
#define CHARGELOG_SIZE   2048

#define SMB_START_CHARGING               (0x40)
#define SMB_STOP_CHARGING                (0x60)
#define QPNP_SMBCHARGER_TIMEOUT          (30000)

enum charge_sysfs_type
{
    CHARGE_SYSFS_IIN_THERMAL = 0,         /* set input current for thermal*/
    CHARGE_SYSFS_IIN_RUNNINGTEST,       /* set input current for runningtest*/
    CHARGE_SYSFS_ENABLE_CHARGER,        /* enabe/disable charger*/
    CHARGE_SYSFS_FACTORY_DIAG_CHARGER,  /* enabe/disable batt charger*/
    CHARGE_SYSFS_RUNNING_TEST_STATUS,    /* for runningtest to get the result*/
    CHARGE_SYSFS_CHARGELOG_HEAD,
    CHARGE_SYSFS_CHARGELOG,
    CHARGE_SYSFS_UPDATE_VOLT_NOW,
    CHARGE_SYSFS_SHIP_MODE,
    CHARGE_SYSFS_IBUS,
};

struct charge_sysfs_data
{
    int iin_rt;
    unsigned int iin_thl;
    char reg_value[CHARGELOG_SIZE];
    char reg_head[CHARGELOG_SIZE];
    struct mutex dump_reg_lock;
    struct mutex dump_reg_head_lock;
};
struct charge_device_ops
{
    int (*set_factory_diag_charger)(int value);   /* for running test charging control*/
    int (*set_enable_charger)(int value); /* for factory diag function */
    int (*set_in_thermal)(int value); /* for power genius to set current limit */
    int (*get_in_thermal)(void);
    int (*is_otg_mode)(void);
    int (*is_usb_ovp)(void);
    void (*set_chg_lock)(unsigned int value);
    int (*set_ship_mode)(int val, bool factory_flag);/*for ship mode function*/
    int (*get_aicl_status)(void);
    int (*get_aicl_level)(void);
    int (*get_iin_max)(void);
	void (*dump_register)(char *reg_value);
	void (*get_register_head)(char *reg_head);
};
struct charge_device_info
{
    struct device   *dev;
    struct charge_sysfs_data sysfs_data;
    struct spmi_device      *spmi;
    struct power_supply     *usb_psy;
    struct power_supply     *batt_psy;
    struct power_supply      *bms_psy;
    struct charge_device_ops *ops;
    int          running_test_settled_status;
    int          chrg_config;
    int          factory_diag;
    //delete the nouse code
    int ship_mode;
#ifdef CONFIG_LOG_JANK
    struct work_struct      usbin_janklog_work;
#endif
    unsigned int            input_event;
    unsigned long           event;
    struct delayed_work     smb_charger_work;
    struct delayed_work chg_adaptive_work;
};

/****************variable and function declarationn area******************/
extern int is_usb_chg_exist(void);
extern int is_usb_ovp(void);
extern int charge_ops_register(struct charge_device_ops *ops);
extern void strncat_length_protect(char * dest, char* src);
extern  int huawei_handle_charger_event(void);
#endif
