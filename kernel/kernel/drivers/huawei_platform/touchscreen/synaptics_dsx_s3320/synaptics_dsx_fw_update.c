/*Add synaptics new driver "Synaptics DSX I2C V2.0"*/
/*
 * Synaptics DSX touchscreen driver
 *
 * Copyright (C) 2012 Synaptics Incorporated
 *
 * Copyright (C) 2012 Alexandra Chin <alexandra.chin@tw.synaptics.com>
 * Copyright (C) 2012 Scott Lin <scott.lin@tw.synaptics.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/pm_runtime.h>
#include <linux/gpio.h>
#include "synaptics_dsx.h"
#include "synaptics_dsx_i2c.h"
#ifdef CONFIG_HUAWEI_KERNEL
#ifdef CONFIG_APP_INFO
#include <misc/app_info.h>
#endif /*CONFIG_APP_INFO*/
#ifdef CONFIG_HUAWEI_DSM
#include <dsm/dsm_pub.h>
#endif/*CONFIG_HUAWEI_DSM*/
#include "synaptics_dsx_esd.h"
static char touch_info[50] = {0};
#endif /*CONFIG_HUAWEI_KERNEL*/

#define FW_IMAGE_NAME "synaptics/startup_fw_update.img"

#define DO_STARTUP_FW_UPDATE
#define STARTUP_FW_UPDATE_DELAY_MS 1000 /* ms */
#define FORCE_UPDATE false
#define DO_LOCKDOWN false

#define MAX_IMAGE_NAME_LEN 256
#define MAX_FIRMWARE_ID_LEN 10

#define LOCKDOWN_OFFSET 0xb0
#define FW_IMAGE_OFFSET 0x100

#define BOOTLOADER_ID_OFFSET 0
#define BLOCK_NUMBER_OFFSET 0

#define V5_PROPERTIES_OFFSET 2
#define V5_BLOCK_SIZE_OFFSET 3
#define V5_BLOCK_COUNT_OFFSET 5
#define V5_BLOCK_DATA_OFFSET 2

#define V6_PROPERTIES_OFFSET 1
#define V6_BLOCK_SIZE_OFFSET 2
#define V6_BLOCK_COUNT_OFFSET 3
#define V6_BLOCK_DATA_OFFSET 1
#define V6_FLASH_COMMAND_OFFSET 2
#define V6_FLASH_STATUS_OFFSET 3

#define LOCKDOWN_BLOCK_COUNT 5

#define REG_MAP (1 << 0)
#define UNLOCKED (1 << 1)
#define HAS_CONFIG_ID (1 << 2)
#define HAS_PERM_CONFIG (1 << 3)
#define HAS_BL_CONFIG (1 << 4)
#define HAS_DISP_CONFIG (1 << 5)
#define HAS_CTRL1 (1 << 6)

#define UI_CONFIG_AREA 0x00
#define PERM_CONFIG_AREA 0x01
#define BL_CONFIG_AREA 0x02
#define DISP_CONFIG_AREA 0x03

#define CMD_WRITE_FW_BLOCK 0x2
#define CMD_ERASE_ALL 0x3
#define CMD_WRITE_LOCKDOWN_BLOCK 0x4
#define CMD_READ_CONFIG_BLOCK 0x5
#define CMD_WRITE_CONFIG_BLOCK 0x6
#define CMD_ERASE_CONFIG 0x7
#define CMD_ERASE_BL_CONFIG 0x9
#define CMD_ERASE_DISP_CONFIG 0xa
#define CMD_ENABLE_FLASH_PROG 0xf

#define SLEEP_MODE_NORMAL (0x00)
#define SLEEP_MODE_SENSOR_SLEEP (0x01)
#define SLEEP_MODE_RESERVED0 (0x02)
#define SLEEP_MODE_RESERVED1 (0x03)

#define ENABLE_WAIT_MS (1 * 1000)
#define WRITE_WAIT_MS (3 * 1000)
#define ERASE_WAIT_MS (5 * 1000)

#define MIN_SLEEP_TIME_US 50
#define MAX_SLEEP_TIME_US 100

/*For Probability of fw request fail*/
#define SYN_FW_RETRY_TIMES 2

static ssize_t fwu_sysfs_show_image(struct file *data_file,
		struct kobject *kobj, struct bin_attribute *attributes,
		char *buf, loff_t pos, size_t count);

static ssize_t fwu_sysfs_store_image(struct file *data_file,
		struct kobject *kobj, struct bin_attribute *attributes,
		char *buf, loff_t pos, size_t count);

static ssize_t fwu_sysfs_do_reflash_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_write_config_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_read_config_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_config_area_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_image_name_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_image_size_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t fwu_sysfs_block_size_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_firmware_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_configuration_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_perm_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_bl_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_disp_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_firmware_version_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_hw_firmware_update_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t fwu_sysfs_hw_firmware_update_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static int synaptics_get_firmware_version(char *config_id_ptr, unsigned long length);

extern int synaptics_rmi4_irq_enable(struct synaptics_rmi4_data *rmi4_data,bool enable);

enum bl_version {
	V5 = 5,
	V6 = 6,
};

enum flash_area {
	NONE,
	UI_FIRMWARE,
	CONFIG_AREA,
};

enum update_mode {
	NORMAL = 1,
	FORCE = 2,
	LOCKDOWN = 8,
};

struct image_header {
	/* 0x00 - 0x0f */
	unsigned char checksum[4];
	unsigned char reserved_04;
	unsigned char reserved_05;
	unsigned char options_firmware_id:1;
	unsigned char options_contain_bootloader:1;
	unsigned char options_reserved:6;
	unsigned char bootloader_version;
	unsigned char firmware_size[4];
	unsigned char config_size[4];
	/* 0x10 - 0x1f */
	unsigned char product_id[SYNAPTICS_RMI4_PRODUCT_ID_SIZE];
	unsigned char package_id[2];
	unsigned char package_id_revision[2];
	unsigned char product_info[SYNAPTICS_RMI4_PRODUCT_INFO_SIZE];
	/* 0x20 - 0x2f */
	unsigned char reserved_20_2f[16];
	/* 0x30 - 0x3f */
	unsigned char ds_id[16];
	/* 0x40 - 0x4f */
	unsigned char ds_info[10];
	unsigned char reserved_4a_4f[6];
	/* 0x50 - 0x53 */
	unsigned char firmware_id[4];
};

struct image_header_data {
	bool contains_firmware_id;
	unsigned int firmware_id;
	unsigned int checksum;
	unsigned int firmware_size;
	unsigned int config_size;
	unsigned char bootloader_version;
	unsigned char product_id[SYNAPTICS_RMI4_PRODUCT_ID_SIZE + 1];
	unsigned char product_info[SYNAPTICS_RMI4_PRODUCT_INFO_SIZE];
};

struct pdt_properties {
	union {
		struct {
			unsigned char reserved_1:6;
			unsigned char has_bsr:1;
			unsigned char reserved_2:1;
		} __packed;
		unsigned char data[1];
	};
};

struct f01_device_status {
	union {
		struct {
			unsigned char status_code:4;
			unsigned char reserved:2;
			unsigned char flash_prog:1;
			unsigned char unconfigured:1;
		} __packed;
		unsigned char data[1];
	};
};

struct f01_device_control {
	union {
		struct {
			unsigned char sleep_mode:2;
			unsigned char nosleep:1;
			unsigned char reserved:2;
			unsigned char charger_connected:1;
			unsigned char report_rate:1;
			unsigned char configured:1;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_fwu_handle {
	enum bl_version bl_version;
	bool initialized;
	bool program_enabled;
	bool has_perm_config;
	bool has_bl_config;
	bool has_disp_config;
	bool force_update;
	bool in_flash_prog_mode;
	bool do_lockdown;
	unsigned int data_pos;
	unsigned int image_size;
	unsigned char *image_name;
	unsigned char *ext_data_source;
	unsigned char *read_config_buf;
	unsigned char intr_mask;
	unsigned char command;
	unsigned char bootloader_id[2];
	unsigned char flash_properties;
	unsigned char flash_status;
	unsigned char productinfo1;
	unsigned char productinfo2;
	unsigned char properties_off;
	unsigned char blk_size_off;
	unsigned char blk_count_off;
	unsigned char blk_data_off;
	unsigned char flash_cmd_off;
	unsigned char flash_status_off;
	unsigned short block_size;
	unsigned short fw_block_count;
	unsigned short config_block_count;
	unsigned short lockdown_block_count;
	unsigned short perm_config_block_count;
	unsigned short bl_config_block_count;
	unsigned short disp_config_block_count;
	unsigned short config_size;
	unsigned short config_area;
	char product_id[SYNAPTICS_RMI4_PRODUCT_ID_SIZE + 1];
	const unsigned char *firmware_data;
	const unsigned char *config_data;
	const unsigned char *lockdown_data;
	unsigned char *firmware_name;
	struct workqueue_struct *fwu_workqueue;
	struct delayed_work fwu_work;
	struct synaptics_rmi4_fn_desc f34_fd;
	struct synaptics_rmi4_access_ptr *fn_ptr;
	struct synaptics_rmi4_data *rmi4_data;
};

/*To fix CTS issue*/
static struct bin_attribute dev_attr_data = {
	.attr = {
		.name = "data",
		.mode = (S_IRUGO | S_IWUSR|S_IWGRP),
	},
	.size = 0,
	.read = fwu_sysfs_show_image,
	.write = fwu_sysfs_store_image,
};

static struct device_attribute attrs[] = {
	__ATTR(doreflash, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_do_reflash_store),
	__ATTR(writeconfig, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_write_config_store),
	__ATTR(readconfig, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_read_config_store),
	__ATTR(configarea, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_config_area_store),
	__ATTR(imagename, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_image_name_store),
	__ATTR(imagesize, S_IWUSR|S_IWGRP,
			synaptics_rmi4_show_error,
			fwu_sysfs_image_size_store),
	__ATTR(blocksize, S_IRUGO,
			fwu_sysfs_block_size_show,
			synaptics_rmi4_store_error),
	__ATTR(fwblockcount, S_IRUGO,
			fwu_sysfs_firmware_block_count_show,
			synaptics_rmi4_store_error),
	__ATTR(configblockcount, S_IRUGO,
			fwu_sysfs_configuration_block_count_show,
			synaptics_rmi4_store_error),
	__ATTR(permconfigblockcount, S_IRUGO,
			fwu_sysfs_perm_config_block_count_show,
			synaptics_rmi4_store_error),
	__ATTR(blconfigblockcount, S_IRUGO,
			fwu_sysfs_bl_config_block_count_show,
			synaptics_rmi4_store_error),
	__ATTR(dispconfigblockcount, S_IRUGO,
			fwu_sysfs_disp_config_block_count_show,
			synaptics_rmi4_store_error),
	__ATTR(firmwareVersion, S_IRUGO,
			fwu_sysfs_firmware_version_show,
			synaptics_rmi4_store_error),
	__ATTR(hwFirmwareUpdate, S_IRUGO | S_IWUSR,
			fwu_sysfs_hw_firmware_update_show,
			fwu_sysfs_hw_firmware_update_store),
};

static struct synaptics_rmi4_fwu_handle *fwu;

DECLARE_COMPLETION(fwup_remove_complete);
/* fw err infomation: err number */
#ifdef CONFIG_HUAWEI_DSM
ssize_t synaptics_dsm_record_fw_err_info( int err_numb )
{

	ssize_t size = 0;
	ssize_t total_size = 0;
	struct dsm_client *tp_dclient = tp_dsm_get_client();

	/* fw upgrad err number */
	size =dsm_client_record(tp_dclient, "fw upgrad err number:%d\n", err_numb );
	total_size += size;

	/* test whether BL is ok */


	return total_size;

}

/* F34 read pdt err infomation: err number */
static struct synaptics_rmi4_fn_desc *g_rmi_fd = NULL;
ssize_t synaptics_dsm_f34_pdt_err_info( int err_numb )
{

	ssize_t size = 0;
	ssize_t total_size = 0;
	struct dsm_client *tp_dclient = tp_dsm_get_client();

	/* F34 read pdt err number */
	size =dsm_client_record(tp_dclient, "F34 read pdt err number:%d\n", err_numb );
	total_size += size;
	
	/* F34 record pdt err info */
	if(NULL != g_rmi_fd)
	{
		dsm_client_record(tp_dclient, 
					"struct synaptics_rmi4_fn_desc{\n"
					" query_base_addr       :%d\n"
					" cmd_base_addr         :%d\n"
					" ctrl_base_addr        :%d\n"
					" data_base_addr        :%d\n"
					" intr_src_count(3)     :%d\n"
					" fn_number             :%d\n"
					"}\n",
					g_rmi_fd->query_base_addr,
					g_rmi_fd->cmd_base_addr,
					g_rmi_fd->ctrl_base_addr,
					g_rmi_fd->data_base_addr,
					g_rmi_fd->intr_src_count,
					g_rmi_fd->fn_number);
		total_size += size;
	}

	return total_size;
}
/* fwu init read pdt props: err number */
ssize_t synaptics_dsm_fwu_init_pdt_props_err_info( int err_numb )
{

	ssize_t size = 0;
	ssize_t total_size = 0;
	struct dsm_client *tp_dclient = tp_dsm_get_client();

	/* fwu init read pdt props err number*/
	size =dsm_client_record(tp_dclient, "fwu init read pdt props err number:%d\n", err_numb );
	total_size += size;

	return total_size;
}
/* f34 read queries: err number */
ssize_t synaptics_dsm_f34_read_queries_err_info( int err_numb )
{

	ssize_t size = 0;
	ssize_t total_size = 0;
	struct dsm_client *tp_dclient = tp_dsm_get_client();

	/* f34 read queries err number*/
	size =dsm_client_record(tp_dclient, "f34 read queries err number:%d\n", err_numb );
	total_size += size;
	/* F34 record bootloader info */
	if(NULL != fwu)
	{
		size =dsm_client_record(tp_dclient, "fwu->bootloader_id[1]='%c'\n", fwu->bootloader_id[1]);
		total_size += size;
	}

	return total_size;
}
#endif/*CONFIG_HUAWEI_DSM*/

static unsigned int extract_uint_le(const unsigned char *ptr)
{
	return (unsigned int)ptr[0] +
			(unsigned int)ptr[1] * 0x100 +
			(unsigned int)ptr[2] * 0x10000 +
			(unsigned int)ptr[3] * 0x1000000;
}

static unsigned int extract_uint_be(const unsigned char *ptr)
{
	return (unsigned int)ptr[3] +
			(unsigned int)ptr[2] * 0x100 +
			(unsigned int)ptr[1] * 0x10000 +
			(unsigned int)ptr[0] * 0x1000000;
}

static void parse_header(struct image_header_data *header,
		const unsigned char *fw_image)
{
	struct image_header *data = (struct image_header *)fw_image;

	header->checksum = extract_uint_le(data->checksum);

	header->bootloader_version = data->bootloader_version;

	header->firmware_size = extract_uint_le(data->firmware_size);

	header->config_size = extract_uint_le(data->config_size);

	memcpy(header->product_id, data->product_id, sizeof(data->product_id));
	header->product_id[sizeof(data->product_id)] = 0;

	memcpy(header->product_info, data->product_info,
			sizeof(data->product_info));

	header->contains_firmware_id = data->options_firmware_id;
	if (header->contains_firmware_id)
		header->firmware_id = extract_uint_le(data->firmware_id);

	return;
}

static int fwu_read_f01_device_status(struct f01_device_status *status)
{
	int retval;
	struct synaptics_rmi4_data *rmi4_data = fwu->rmi4_data;

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			rmi4_data->f01_data_base_addr,
			status->data,
			sizeof(status->data));
	if (retval < 0) {
		tp_log_err("%s: Failed to read F01 device status\n",
				__func__);
		return retval;
	}

	return 0;
}

static int fwu_read_f34_queries(void)
{
	int retval;
	unsigned char count;
	unsigned char buf[10];
	int blid_read_retry = 5;

blid_read_again:
	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.query_base_addr + BOOTLOADER_ID_OFFSET,
			fwu->bootloader_id,
			sizeof(fwu->bootloader_id));
	if (retval < 0) {
		tp_log_err("%s: Failed to read bootloader ID\n",
				__func__);
		return retval;
	}

	if (fwu->bootloader_id[1] == '5') {
		fwu->bl_version = V5;
	} else if (fwu->bootloader_id[1] == '6') {
		fwu->bl_version = V6;
	} else {
		if(blid_read_retry > 0) {
			blid_read_retry --;
			msleep(20);
			tp_log_err("%s: retry left(%d) to read bi_id \n",__func__,blid_read_retry);
			goto blid_read_again;
		} else {
			tp_log_err("%s: Unrecognized bootloader version('%c')\n",
				__func__,fwu->bootloader_id[1]);
			return -EINVAL;
		}
	}

	if (fwu->bl_version == V5) {
		fwu->properties_off = V5_PROPERTIES_OFFSET;
		fwu->blk_size_off = V5_BLOCK_SIZE_OFFSET;
		fwu->blk_count_off = V5_BLOCK_COUNT_OFFSET;
		fwu->blk_data_off = V5_BLOCK_DATA_OFFSET;
	} else if (fwu->bl_version == V6) {
		fwu->properties_off = V6_PROPERTIES_OFFSET;
		fwu->blk_size_off = V6_BLOCK_SIZE_OFFSET;
		fwu->blk_count_off = V6_BLOCK_COUNT_OFFSET;
		fwu->blk_data_off = V6_BLOCK_DATA_OFFSET;
	}

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.query_base_addr + fwu->properties_off,
			&fwu->flash_properties,
			sizeof(fwu->flash_properties));
	if (retval < 0) {
		tp_log_err("%s: Failed to read flash properties\n",
				__func__);
		return retval;
	}

	count = 4;

	if (fwu->flash_properties & HAS_PERM_CONFIG) {
		fwu->has_perm_config = 1;
		count += 2;
	}

	if (fwu->flash_properties & HAS_BL_CONFIG) {
		fwu->has_bl_config = 1;
		count += 2;
	}

	if (fwu->flash_properties & HAS_DISP_CONFIG) {
		fwu->has_disp_config = 1;
		count += 2;
	}

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.query_base_addr + fwu->blk_size_off,
			buf,
			2);
	if (retval < 0) {
		tp_log_err("%s: Failed to read block size info\n",
				__func__);
		return retval;
	}

	batohs(&fwu->block_size, &(buf[0]));

	if (fwu->bl_version == V5) {
		fwu->flash_cmd_off = fwu->blk_data_off + fwu->block_size;
		fwu->flash_status_off = fwu->flash_cmd_off;
	} else if (fwu->bl_version == V6) {
		fwu->flash_cmd_off = V6_FLASH_COMMAND_OFFSET;
		fwu->flash_status_off = V6_FLASH_STATUS_OFFSET;
	}

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.query_base_addr + fwu->blk_count_off,
			buf,
			count);
	if (retval < 0) {
		tp_log_err("%s: Failed to read block count info\n",
				__func__);
		return retval;
	}

	batohs(&fwu->fw_block_count, &(buf[0]));
	batohs(&fwu->config_block_count, &(buf[2]));

	count = 4;

	if (fwu->has_perm_config) {
		batohs(&fwu->perm_config_block_count, &(buf[count]));
		count += 2;
	}

	if (fwu->has_bl_config) {
		batohs(&fwu->bl_config_block_count, &(buf[count]));
		count += 2;
	}

	if (fwu->has_disp_config)
		batohs(&fwu->disp_config_block_count, &(buf[count]));

	return 0;
}

static int fwu_read_f34_flash_status(void)
{
	int retval;
	unsigned char status;
	unsigned char command;

	/* delete log */
	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + fwu->flash_status_off,
			&status,
			sizeof(status));
	if (retval < 0) {
		tp_log_err("%s(line %d): "
				"Failed to read flash status\n",
				__func__,__LINE__);
		return retval;
	}

	fwu->program_enabled = status >> 7;

	if (fwu->bl_version == V5)
		fwu->flash_status = (status >> 4) & MASK_3BIT;
	else if (fwu->bl_version == V6)
		fwu->flash_status = status & MASK_3BIT;

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + fwu->flash_cmd_off,
			&command,
			sizeof(command));
	if (retval < 0) {
		tp_log_err("%s(line %d): "
				" Failed to read flash command\n",
				__func__,__LINE__);
		return retval;
	}

	fwu->command = command & MASK_4BIT;

	return 0;
}

static int fwu_write_f34_command(unsigned char cmd)
{
	int retval;
	unsigned char command = cmd & MASK_4BIT;

	fwu->command = cmd;

	retval = fwu->fn_ptr->write(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + fwu->flash_cmd_off,
			&command,
			sizeof(command));
	if (retval < 0) {
		tp_log_err("%s(line %d): "
			"Failed to write command 0x%02x\n",
			__func__,__LINE__, command);
		return retval;
	}

	return 0;
}
static int time_out_count = 0;
static int fwu_wait_for_idle(int timeout_ms)
{
	int retval = 0;
	long int irq_count = 0;
	unsigned char data[MAX_INTR_REGISTERS + 1];
	int count = 0;
	int irq_gpio_status = -1;
	int timeout_count = ((timeout_ms * 1000) / MAX_SLEEP_TIME_US) + 1;

	do {
		usleep_range(MIN_SLEEP_TIME_US, MAX_SLEEP_TIME_US);

		count++;
		retval = fwu_read_f34_flash_status();
		if(retval < 0) {
			tp_log_debug("%s:Failed to read f34 flash status: %d\n", __func__, retval);
		}

		if (count == timeout_count) {
			tp_log_info("%s(line %d): Last time to check status!!!!\n",
							__func__,__LINE__);

			time_out_count += 1;
			irq_gpio_status = gpio_get_value(fwu->rmi4_data->board->irq_gpio);
			irq_count = get_irq_count();
			tp_log_info("%s(line %d): First time read irq_gpio_status=%d, timeout_count=%d, irq_count = %ld\n",
						__func__,__LINE__, irq_gpio_status, time_out_count, irq_count);

			if (irq_gpio_status == 0) {

				// read tp reg status
				retval = fwu->fn_ptr->read(fwu->rmi4_data,
						fwu->rmi4_data->f01_ctrl_base_addr,
						data, 2);
				if (retval < 0) {
					tp_log_err("%s(line %d): Failed to read flash status\n",
						__func__,__LINE__);
					return retval;
				} else {
					tp_log_info("%s %d:dev_ctrl_reg[0]=0x%04x, dev_ctrl_reg[1]=0x%04x\n",
						__func__, __LINE__, data[0], data[1]);
				}
			}

			tp_log_info("%s(line %d): command=0x%04x\n",
							__func__,__LINE__, fwu->command);
			fwu_read_f34_flash_status();
		}

		if ((fwu->command == 0x00) && (fwu->flash_status == 0x00))
			return 0;
	} while (count < timeout_count);

	tp_log_err("%s(line %d): "
		"Timed out waiting for idle status\n",
		__func__,__LINE__);

	return -ETIMEDOUT;
}
static enum flash_area fwu_go_nogo(struct image_header_data *header)
{
	int retval;
	enum flash_area flash_area = NONE;
	unsigned char index = 0;
	unsigned char config_id[4];
	unsigned int device_config_id;
	unsigned int image_config_id;
	unsigned int device_fw_id;
	unsigned long image_fw_id;
	char *strptr;
	char *firmware_id;

	if (fwu->force_update) {
		tp_log_warning("%s(line %d):fwu->force_update = %d\n",
				__func__,__LINE__, fwu->force_update);
		flash_area = UI_FIRMWARE;
		goto exit;
	}

	/* Update both UI and config if device is in bootloader mode */
	if (fwu->in_flash_prog_mode) {
		tp_log_warning("%s(line %d):fwu->in_flash_prog_mode = %d\n",
				__func__,__LINE__, fwu->in_flash_prog_mode);
		flash_area = UI_FIRMWARE;
		goto exit;
	}

	/* Get device firmware ID */
	device_fw_id = fwu->rmi4_data->firmware_id;
	tp_log_warning("%s(line %d): Device firmware ID = %d\n",
			__func__,__LINE__, device_fw_id);

	/* Get image firmware ID */
	if (header->contains_firmware_id) {
		image_fw_id = header->firmware_id;
	} else {
		strptr = strstr(fwu->image_name, "PR");
		if (!strptr) {
			tp_log_err("%s(line %d): "
					"No valid PR number (PRxxxxxxx) "
					"found in image file name (%s)\n",
					__func__,__LINE__, fwu->image_name);
			flash_area = NONE;
			goto exit;
		}

		strptr += 2;
		firmware_id = kzalloc(MAX_FIRMWARE_ID_LEN, GFP_KERNEL);
		while (strptr[index] >= '0' && strptr[index] <= '9') {
			firmware_id[index] = strptr[index];
			index++;
		}

		retval = sstrtoul(firmware_id, 10, &image_fw_id);
		kfree(firmware_id);
		if (retval) {
			tp_log_err("%s(line %d): "
					" Failed to obtain image firmware ID\n",
					__func__,__LINE__);
			flash_area = NONE;
			goto exit;
		}
	}
	tp_log_warning("%s(line %d):Image firmware ID = %d\n",
			__func__,__LINE__, (unsigned int)image_fw_id);

	if (image_fw_id > device_fw_id) {
		flash_area = UI_FIRMWARE;
		goto exit;
	} else if (image_fw_id < device_fw_id) {
		tp_log_warning("%s(line %d): Image firmware ID older than device firmware ID\n",
				__func__,__LINE__);
		flash_area = NONE;
		goto exit;
	} else {
		tp_log_info("%s %d:Image firmware id match device firmware id\n", 
			__func__, __LINE__);
	}

	/* Get device config ID */
	retval = fwu->fn_ptr->read(fwu->rmi4_data,
				fwu->f34_fd.ctrl_base_addr,
				config_id,
				sizeof(config_id));
	if (retval < 0) {
		tp_log_err("%s(line %d):Failed to read device config ID\n",
				__func__,__LINE__);
		flash_area = NONE;
		goto exit;
	}
	device_config_id = extract_uint_be(config_id);
	tp_log_warning("%s(line %d): "
			"Device config ID = 0x%02x 0x%02x 0x%02x 0x%02x\n",
			__func__,__LINE__,
			config_id[0],
			config_id[1],
			config_id[2],
			config_id[3]);

	/* Get image config ID */
	image_config_id = extract_uint_be(fwu->config_data);
	tp_log_warning("%s(line %d): "
			"Image config ID = 0x%02x 0x%02x 0x%02x 0x%02x\n",
			__func__,__LINE__,
			fwu->config_data[0],
			fwu->config_data[1],
			fwu->config_data[2],
			fwu->config_data[3]);

	if (image_config_id > device_config_id) {
		flash_area = CONFIG_AREA;
 		goto exit;
	}

	flash_area = NONE;

exit:
	if (flash_area == NONE) {
		tp_log_warning("%s(line %d): No need to do reflash\n",
				__func__,__LINE__);
	} else {
		tp_log_warning("%s(line %d): Updating %s\n",
				__func__,__LINE__,
				flash_area == UI_FIRMWARE ? "UI firmware" : "config only");
	}

	return flash_area;
}
static int fwu_scan_pdt(void)
{
	int retval;
	unsigned char ii = 0;
	unsigned char intr_count = 0;
	int pdt_retry = 3; /* used to retry read register to get the right value of f01/f34 */
	unsigned char intr_off;
	unsigned char intr_src;
	unsigned short addr;
	bool f01found = false;
	bool f34found = false;
	struct synaptics_rmi4_fn_desc rmi_fd;
	struct synaptics_rmi4_data *rmi4_data = fwu->rmi4_data;

init_f01_f34:
	for (addr = PDT_START; addr > PDT_END; addr -= PDT_ENTRY_SIZE) {
		retval = fwu->fn_ptr->read(fwu->rmi4_data,
				addr,
				(unsigned char *)&rmi_fd,
				sizeof(rmi_fd));
		if (retval < 0)
		{
			tp_log_err( "%s: Read addr(0x%02x) fail!\n",__func__,addr);
			return retval;
		}

		if (rmi_fd.fn_number) {
			tp_log_info( "%s: Found F%02x\n",__func__, rmi_fd.fn_number);

			switch (rmi_fd.fn_number) {
			case SYNAPTICS_RMI4_F01:
				f01found = true;

				rmi4_data->f01_query_base_addr = rmi_fd.query_base_addr;
				rmi4_data->f01_ctrl_base_addr = rmi_fd.ctrl_base_addr;
				rmi4_data->f01_data_base_addr = rmi_fd.data_base_addr;
				rmi4_data->f01_cmd_base_addr = rmi_fd.cmd_base_addr;
				break;
			case SYNAPTICS_RMI4_F34:
				f34found = true;
				fwu->f34_fd.query_base_addr = rmi_fd.query_base_addr;
				fwu->f34_fd.ctrl_base_addr = rmi_fd.ctrl_base_addr;
				fwu->f34_fd.data_base_addr = rmi_fd.data_base_addr;

				fwu->intr_mask = 0;
				intr_src = rmi_fd.intr_src_count;
				intr_off = intr_count % 8;
				for (ii = intr_off;
						ii < ((intr_src & MASK_3BIT) +
						intr_off);
						ii++) {
					fwu->intr_mask |= 1 << ii;
				}
				break;
			}
		}
		else
		{
			tp_log_info( "%s: Read addr(0x%02x) fn_number is zero!\n",
				__func__,addr);
		/* delete some err log  for pass the mmi test */
			break;
		}

		/* if f01 and f34 is abnormal, end loop to trace error */
		if (((F01_QUERY_BASE == addr) && !f01found) || 
			((F34_QUERY_BASE == addr) && !f34found)) 
		{
			goto pdt_done;
		}

		/* if f01 and f34 is found, end loop for saving time */
		if (f01found && f34found) {
			goto pdt_done;
		}

		intr_count += (rmi_fd.intr_src_count & MASK_3BIT);
	}
pdt_done:
	pdt_retry--;
	/* if f01found or f34found is not set, print the register data, and retry to read the register */
	if (!f01found || !f34found) {
#ifdef CONFIG_HUAWEI_DSM
		g_rmi_fd = &rmi_fd;
#endif/*CONFIG_HUAWEI_DSM*/

		tp_log_err("%s#line %d#\n",__func__,__LINE__);
		tp_log_err("%s addr(0x%02x)\n",__func__,addr);
		tp_log_err("%s ii=%d\n",__func__,ii);
	#ifdef CONFIG_64BIT
		tp_log_err("%s sizeof(rmi_fd):%lu\n",__func__,sizeof(rmi_fd));
	#else
		tp_log_err("%s sizeof(rmi_fd):%d\n",__func__,sizeof(rmi_fd));
	#endif
		tp_log_err("%s struct synaptics_rmi4_fn_desc{\n",__func__);
		tp_log_err("%s  query_base_addr       :0x%02x\n",__func__,rmi_fd.query_base_addr);
		tp_log_err("%s  cmd_base_addr         :0x%02x\n",__func__,rmi_fd.cmd_base_addr);
		tp_log_err("%s  ctrl_base_addr        :0x%02x\n",__func__,rmi_fd.ctrl_base_addr);
		tp_log_err("%s  data_base_addr        :0x%02x\n",__func__,rmi_fd.data_base_addr);
		tp_log_err("%s  intr_src_count(3)     :0x%02x\n",__func__,rmi_fd.intr_src_count);
		tp_log_err("%s  fn_number             :0x%02x\n",__func__,rmi_fd.fn_number);
		tp_log_err("%s }\n",__func__);

		tp_log_err("%s: line(%d) PDT scan Error f01found:%d f34found:%d\n", __func__,__LINE__,f01found,f34found);

		/* retry to read the register to get the right value */
		if (pdt_retry) {
			tp_log_err("%s: retry(%d) to scan pdt \n",__func__,pdt_retry);
			goto init_f01_f34;
		}

		/* report to device_monitor */
#ifdef CONFIG_HUAWEI_DSM
		synp_tp_report_dsm_err(DSM_TP_F34_PDT_ERROR_NO, retval);
#endif/*CONFIG_HUAWEI_DSM*/

		/*delete the app info set here*/
		return -EINVAL;
	} else {
		tp_log_info("%s: line(%d) f01/f34 found.\n",__func__,__LINE__);
	}

	return 0;
}
static int fwu_write_blocks(unsigned char *block_ptr, unsigned short block_cnt,
		unsigned char command)
{
	int retval;
	long int old_irq_count = 0;
	long int new_irq_count = 0;
	unsigned char block_offset[] = {0, 0};
	unsigned short block_num;

	block_offset[1] |= (fwu->config_area << 5);

	tp_log_info("%s: Enter, block_cnt=%d, block_size=%d\n", __func__, 
		block_cnt, fwu->block_size);
	retval = fwu->fn_ptr->write(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + BLOCK_NUMBER_OFFSET,
			block_offset,
			sizeof(block_offset));
	if (retval < 0) {
		tp_log_err("%s: Failed to write to block number registers\n",
				__func__);
		return retval;
	}

	for (block_num = 0; block_num < block_cnt; block_num++) {

		old_irq_count = get_irq_count();

		retval = fwu->fn_ptr->write(fwu->rmi4_data,
				fwu->f34_fd.data_base_addr + fwu->blk_data_off,
				block_ptr,
				fwu->block_size);
		if (retval < 0) {
			tp_log_err("%s: Failed to write block data (block %d)\n",
					__func__, block_num);
			return retval;
		}

		retval = fwu_write_f34_command(command);
		if (retval < 0) {
			tp_log_err("%s: Failed to write command for block %d\n",
					__func__, block_num);
			return retval;
		}

		retval = fwu_wait_for_idle(WRITE_WAIT_MS);
		if (retval < 0) {
			tp_log_err("%s: Failed to wait for idle status (block %d)\n",
					__func__, block_num);
			return retval;
		}

		new_irq_count = get_irq_count();
		if (new_irq_count - old_irq_count == 0) {
			tp_log_info("%s: block_num=%d\n", __func__, block_num);
			tp_log_info("%s(%d): new_irq_count = %ld; old_irq_count = %ld; sub=%ld; block_num=%d \n",
				__func__, __LINE__, new_irq_count, old_irq_count, new_irq_count - old_irq_count, block_num);
		}

		block_ptr += fwu->block_size;
	}

	tp_log_info("%s: Exit\n", __func__);

	return 0;
}
static int fwu_write_firmware(void)
{
	return fwu_write_blocks((unsigned char *)fwu->firmware_data,
		fwu->fw_block_count, CMD_WRITE_FW_BLOCK);
}

static int fwu_write_configuration(void)
{
	return fwu_write_blocks((unsigned char *)fwu->config_data,
		fwu->config_block_count, CMD_WRITE_CONFIG_BLOCK);
}

static int fwu_write_lockdown(void)
{
	return fwu_write_blocks((unsigned char *)fwu->lockdown_data,
		fwu->lockdown_block_count, CMD_WRITE_LOCKDOWN_BLOCK);
}

static int fwu_write_bootloader_id(void)
{
	int retval;

	retval = fwu->fn_ptr->write(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + fwu->blk_data_off,
			fwu->bootloader_id,
			sizeof(fwu->bootloader_id));
	if (retval < 0) {
		tp_log_err("%s(line %d): "
			"Failed to write bootloader ID\n",
			__func__,__LINE__);
		return retval;
	}

	return 0;
}

static int fwu_enter_flash_prog(void)
{
	int retval;
	struct f01_device_status f01_device_status;
	struct f01_device_control f01_device_control;
	struct synaptics_rmi4_data *rmi4_data = fwu->rmi4_data;

	tp_log_warning("%s(line %d): ", __func__, __LINE__);
	retval = fwu_write_bootloader_id();
	tp_log_warning("%s(line %d):fwu_write_bootloader_id ,retval=%d ",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	retval = fwu_write_f34_command(CMD_ENABLE_FLASH_PROG);
	tp_log_warning("%s(line %d):fwu_write_f34_command ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	retval = fwu_wait_for_idle(ENABLE_WAIT_MS);
	tp_log_warning("%s(line %d):fwu_wait_for_idle ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	if (!fwu->program_enabled) {
		tp_log_err("%s: Program enabled bit not set\n",
				__func__);
		return -EINVAL;
	}

	retval = fwu_scan_pdt();
	tp_log_warning("%s(line %d):fwu_scan_pdt ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	retval = fwu_read_f01_device_status(&f01_device_status);
	tp_log_warning("%s(line %d):fwu_read_f01_device_status ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	if (!f01_device_status.flash_prog) {
		tp_log_err("%s: Not in flash prog mode\n",
				__func__);
		return -EINVAL;
	}

	retval = fwu_read_f34_queries();
	tp_log_warning("%s(line %d):fwu_read_f34_queries ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)
		return retval;

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			f01_device_control.data,
			sizeof(f01_device_control.data));
	tp_log_warning("%s(line %d): read,retval=%d",__func__,__LINE__,retval);
	if (retval < 0) {
		tp_log_err("%s: Failed to read F01 device control\n",
				__func__);
		return retval;
	}

	f01_device_control.nosleep = true;
	f01_device_control.sleep_mode = SLEEP_MODE_NORMAL;

	retval = fwu->fn_ptr->write(fwu->rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			f01_device_control.data,
			sizeof(f01_device_control.data));
	tp_log_warning("%s(line %d): write,retval=%d",__func__,__LINE__,retval);
	if (retval < 0) {
		tp_log_err("%s: Failed to write F01 device control\n",
				__func__);
		return retval;
	}

	return retval;
}
static void print_irq_gpio_status_and_count(void)
{
	int irq_status = 0;
	long int irq_count = 0;
	
	irq_status = gpio_get_value(fwu->rmi4_data->board->irq_gpio);
	irq_count = get_irq_count();

	tp_log_warning("%s(line %d):irq status=%d, irq_count=%ld\n",
			__func__,__LINE__, irq_status, irq_count);
}

static int fwu_do_reflash(void)
{
	int retval;


	retval = fwu_enter_flash_prog();
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d):Entered flash prog mode\n", 
		__func__,__LINE__);

	retval = fwu_write_bootloader_id();
	if (retval < 0) {
		tp_log_err("%s(line %d):write bl fail, ret=%d\n", 
			__func__,__LINE__, retval);
		return retval;
	}

	tp_log_warning("%s(line %d): Bootloader ID written\n",
			__func__,__LINE__);


	print_irq_gpio_status_and_count();
	retval = fwu_write_f34_command(CMD_ERASE_ALL);
	if (retval < 0) {
		tp_log_err("%s(line %d):write f43 fail, ret=%d\n", 
			__func__,__LINE__, retval);
		return retval;
	}
	tp_log_warning("%s(line %d):Erase all command written\n",
			__func__,__LINE__);

	print_irq_gpio_status_and_count();

	time_out_count = 0;

	/* delay for erase ic data finish */
	msleep(2000);

	retval = fwu_wait_for_idle(ERASE_WAIT_MS);
	if (retval < 0) {
		tp_log_err("%s(line %d):wait for idle fail, ret=%d\n", 
					__func__,__LINE__, retval);
		return retval;
	}
	tp_log_warning("%s(line %d):Idle status detected\n",
			__func__,__LINE__);
	print_irq_gpio_status_and_count();

	if (fwu->firmware_data) {
		tp_log_info("%s(line %d):fwu_write_firmware\n",__func__,__LINE__);
		retval = fwu_write_firmware();
		if (retval < 0) {
			tp_log_err("%s(line %d):write firmware fail, ret=%d\n", 
				__func__,__LINE__, retval);
			return retval;
		}
		tp_log_warning("%s(line %d): Firmware programmed\n", __func__,__LINE__);
	}

	if (fwu->config_data) {
		tp_log_info("%s(line %d):fwu_write_configuration\n",__func__,__LINE__);
		retval = fwu_write_configuration();
		if (retval < 0) {
			tp_log_err("%s(line %d):write configuration fail, ret=%d\n", 
				__func__,__LINE__, retval);
			return retval;
		}
		tp_log_warning("%s(line %d): Configuration programmed\n", __func__,__LINE__);
	}

	return retval;
}
static int fwu_do_write_config(void)
{
	int retval;

	retval = fwu_enter_flash_prog();
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d): "
			"Entered flash prog mode\n",
			__func__,__LINE__);

	if (fwu->config_area == PERM_CONFIG_AREA) {
		fwu->config_block_count = fwu->perm_config_block_count;
		goto write_config;
	}

	retval = fwu_write_bootloader_id();
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d): "
			"Bootloader ID written\n",
			__func__,__LINE__);

	switch (fwu->config_area) {
	case UI_CONFIG_AREA:
		retval = fwu_write_f34_command(CMD_ERASE_CONFIG);
		break;
	case BL_CONFIG_AREA:
		retval = fwu_write_f34_command(CMD_ERASE_BL_CONFIG);
		fwu->config_block_count = fwu->bl_config_block_count;
		break;
	case DISP_CONFIG_AREA:
		retval = fwu_write_f34_command(CMD_ERASE_DISP_CONFIG);
		fwu->config_block_count = fwu->disp_config_block_count;
		break;
	}
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d): "
			"Erase command written\n",
			__func__,__LINE__);

	retval = fwu_wait_for_idle(ERASE_WAIT_MS);
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d): "
			"Idle status detected\n",
			__func__,__LINE__);

write_config:
	retval = fwu_write_configuration();
	if (retval < 0)
		return retval;

	tp_log_warning("%s(line %d): Config written\n", __func__,__LINE__);

	return retval;
}

static int fwu_start_write_config(void)
{
	int retval;
	unsigned short block_count;
	struct image_header_data header;

	switch (fwu->config_area) {
	case UI_CONFIG_AREA:
		block_count = fwu->config_block_count;
		break;
	case PERM_CONFIG_AREA:
		if (!fwu->has_perm_config)
			return -EINVAL;
		block_count = fwu->perm_config_block_count;
		break;
	case BL_CONFIG_AREA:
		if (!fwu->has_bl_config)
			return -EINVAL;
		block_count = fwu->bl_config_block_count;
		break;
	case DISP_CONFIG_AREA:
		if (!fwu->has_disp_config)
			return -EINVAL;
		block_count = fwu->disp_config_block_count;
		break;
	default:
		return -EINVAL;
	}

	if (fwu->ext_data_source)
		fwu->config_data = fwu->ext_data_source;
	else
		return -EINVAL;

	fwu->config_size = fwu->block_size * block_count;

	/* Jump to the config area if given a packrat image */
	if ((fwu->config_area == UI_CONFIG_AREA) &&
			(fwu->config_size != fwu->image_size)) {
		parse_header(&header, fwu->ext_data_source);

		if (header.config_size) {
			fwu->config_data = fwu->ext_data_source +
					FW_IMAGE_OFFSET +
					header.firmware_size;
		} else {
			return -EINVAL;
		}
	}

	tp_log_info("%s: Start of write config process\n", __func__);

	retval = fwu_do_write_config();
	if (retval < 0) {
		tp_log_err("%s: Failed to write config\n",
				__func__);
	}

	fwu->rmi4_data->reset_device(fwu->rmi4_data);

	tp_log_info("%s: End of write config process\n", __func__);

	return retval;
}

static int fwu_do_read_config(void)
{
	int retval;
	unsigned char block_offset[] = {0, 0};
	unsigned short block_num;
	unsigned short block_count;
	unsigned short index = 0;

	retval = fwu_enter_flash_prog();
	if (retval < 0)
		goto exit;

	tp_log_debug("%s: Entered flash prog mode\n",
			__func__);

	switch (fwu->config_area) {
	case UI_CONFIG_AREA:
		block_count = fwu->config_block_count;
		break;
	case PERM_CONFIG_AREA:
		if (!fwu->has_perm_config) {
			retval = -EINVAL;
			goto exit;
		}
		block_count = fwu->perm_config_block_count;
		break;
	case BL_CONFIG_AREA:
		if (!fwu->has_bl_config) {
			retval = -EINVAL;
			goto exit;
		}
		block_count = fwu->bl_config_block_count;
		break;
	case DISP_CONFIG_AREA:
		if (!fwu->has_disp_config) {
			retval = -EINVAL;
			goto exit;
		}
		block_count = fwu->disp_config_block_count;
		break;
	default:
		retval = -EINVAL;
		goto exit;
	}

	fwu->config_size = fwu->block_size * block_count;

	kfree(fwu->read_config_buf);
	fwu->read_config_buf = kzalloc(fwu->config_size, GFP_KERNEL);

	block_offset[1] |= (fwu->config_area << 5);

	retval = fwu->fn_ptr->write(fwu->rmi4_data,
			fwu->f34_fd.data_base_addr + BLOCK_NUMBER_OFFSET,
			block_offset,
			sizeof(block_offset));
	if (retval < 0) {
		tp_log_err("%s: Failed to write to block number registers\n",
				__func__);
		goto exit;
	}

	for (block_num = 0; block_num < block_count; block_num++) {
		retval = fwu_write_f34_command(CMD_READ_CONFIG_BLOCK);
		if (retval < 0) {
			tp_log_err("%s: Failed to write read config command\n",
					__func__);
			goto exit;
		}

		retval = fwu_wait_for_idle(WRITE_WAIT_MS);
		if (retval < 0) {
			tp_log_err("%s: Failed to wait for idle status\n",
					__func__);
			goto exit;
		}

		retval = fwu->fn_ptr->read(fwu->rmi4_data,
				fwu->f34_fd.data_base_addr + fwu->blk_data_off,
				&fwu->read_config_buf[index],
				fwu->block_size);
		if (retval < 0) {
			tp_log_err("%s: Failed to read block data (block %d)\n",
					__func__, block_num);
			goto exit;
		}

		index += fwu->block_size;
	}

exit:
	fwu->rmi4_data->reset_device(fwu->rmi4_data);

	return retval;
}

static int fwu_do_lockdown(void)
{
	int retval;

	retval = fwu_enter_flash_prog();
	if (retval < 0)
		return retval;

	retval = fwu->fn_ptr->read(fwu->rmi4_data,
			fwu->f34_fd.query_base_addr + fwu->properties_off,
			&fwu->flash_properties,
			sizeof(fwu->flash_properties));
	if (retval < 0) {
		tp_log_err("%s: Failed to read flash properties\n",
				__func__);
		return retval;
	}

	if ((fwu->flash_properties & UNLOCKED) == 0) {
		tp_log_info("%s: Device already locked down\n",
				__func__);
		return retval;
	}

	retval = fwu_write_lockdown();
	if (retval < 0)
		return retval;

	tp_log_info("%s: Lockdown programmed\n", __func__);

	return retval;
}

#ifdef CONFIG_HUAWEI_KERNEL
typedef enum 
{
   TP_COF_ID_OFILM = 0x0006, //ID0 low   , ID1  low
   TP_COF_ID_JUNDA = 0x000A, //ID0 float , ID1  low
   TP_COF_ID_TRULY = 0x0003, //ID1 float , ID0  low
   TP_COF_ID_OTHER = 0x0000, //ID0 float , ID1  float
} hw_tp_id_index;

const char * get_cof_module_name(char * product_id_ptr)
{
	long product_id = UNKNOW_PRODUCT_MODULE;
	
	product_id = get_module_id_by_string(product_id_ptr);

	tp_log_info("%s(%d):product id=%ld\n", __func__, __LINE__, product_id);
	return get_module_name_by_id(product_id);
}

static void synaptics_set_appinfo(void)
{
	int ret = -1;
	unsigned char config_id[4];
	unsigned int device_config_id;

	if (!fwu) {
		tp_log_err("%s:fwu is NULL\n", __func__);
		return;
	}
	/* Get device config ID */
	ret = fwu->fn_ptr->read(fwu->rmi4_data,
				fwu->f34_fd.ctrl_base_addr,
				config_id,
				sizeof(config_id));
	if (ret < 0) {
		tp_log_err("%s(line %d):Failed to read device config ID,ret=%d\n",
				__func__,__LINE__, ret);
		/*add the TP app_info for pass the mmi test*/
		app_info_set("touch_panel", "synaptics_dsx_X.X");
		goto exit;
	}
	device_config_id = extract_uint_be(config_id);
	tp_log_warning("%s(line %d):Device config ID = 0x%02x 0x%02x 0x%02x 0x%02x\n",
			__func__,__LINE__,
			config_id[0],
			config_id[1],
			config_id[2],
			config_id[3]);
	
	snprintf(touch_info, sizeof(touch_info),
		"synaptics_dsx_%s.%x%02x",
		get_cof_module_name(fwu->product_id), 
		config_id[2], 
		config_id[3]);
#ifdef CONFIG_APP_INFO
	ret = app_info_set("touch_panel", touch_info);
	if (ret < 0) {
		tp_log_err("%s(line %d): error,ret=%d\n",__func__,__LINE__,ret);
		goto exit;
	}
#endif /*CONFIG_APP_INFO*/
	
exit:
	return;
}
#endif /*CONFIG_HUAWEI_KERNEL*/

static int fwu_start_reflash(void)
{
	int retval = 0;
	enum flash_area flash_area;
	struct image_header_data header;
	struct f01_device_status f01_device_status;
	const unsigned char *fw_image;
	const struct firmware *fw_entry = NULL;
	/*For Probability of fw request fail*/
	unsigned char retry;

	tp_log_warning("%s(line %d)",__func__,__LINE__);
	/* disable irq until finish fw update work */
	disable_irq_nosync(fwu->rmi4_data->irq);
	pm_runtime_get_sync(&fwu->rmi4_data->i2c_client->dev);
	fwu->rmi4_data->firmware_updating = true;

	tp_log_warning("%s(line %d): Start of reflash process\n", __func__,__LINE__);

	if (fwu->ext_data_source) {
		fw_image = fwu->ext_data_source;
	} else {
#ifndef CONFIG_HUAWEI_KERNEL
		strncpy(fwu->image_name, FW_IMAGE_NAME, MAX_IMAGE_NAME_LEN);
#else /*CONFIG_HUAWEI_KERNEL*/
		strncpy(fwu->image_name, fwu->firmware_name, MAX_IMAGE_NAME_LEN);
#endif /*CONFIG_HUAWEI_KERNEL*/
		tp_log_warning("%s(line %d): Requesting firmware image %s\n",
				__func__,__LINE__, fwu->image_name);

		/*For Probability of fw request fail*/
		for (retry = 0; retry < SYN_FW_RETRY_TIMES; retry++) {
			retval = request_firmware(&fw_entry, fwu->image_name,
					&fwu->rmi4_data->i2c_client->dev);
			if (retval != 0) {
				tp_log_err("%s %d:Firmware image %s not available,retval=%d\n",
						__func__, __LINE__, fwu->image_name, retval);
				retval = -EINVAL;
			}

			if ((NULL != fw_entry)&&(NULL != fw_entry->data)) {
				fw_image = fw_entry->data;
				break;
			}

			tp_log_warning("%s: request_firmware retry %d\n", __func__, retry + 1);
		}

		if (retry == SYN_FW_RETRY_TIMES) {
			tp_log_err("%s: request_firmware fail, over retry limit!retval=%d\n", __func__, retval);
			retval = -EINVAL;
			enable_irq(fwu->rmi4_data->irq);
			goto exit;
		}

		tp_log_warning("%s(line %d): Firmware image %s is get successful!\n",
				__func__,__LINE__, fwu->image_name);
		tp_log_warning("%s(line %d): Firmware image size = %lu\n",
				__func__,__LINE__, (unsigned long)(fw_entry->size));
	}

	parse_header(&header, fw_image);

	tp_log_warning("%s(line %d): fwu->bl_version = %d"
			"header.bootloader_version = %d \n",
			__func__, __LINE__, fwu->bl_version, header.bootloader_version);
	if (fwu->bl_version != header.bootloader_version) {
		tp_log_err("%s(line %d):Bootloader version mismatch\n",
				__func__,__LINE__);
		retval = -EINVAL;
		enable_irq(fwu->rmi4_data->irq);
		goto exit;
	}

	retval = fwu_read_f01_device_status(&f01_device_status);
	/* not need enable irq anymore */
	if (retval < 0) {
		tp_log_err("%s %d:read device status fail, ret=%d\n", 
			__func__, __LINE__, retval);
		enable_irq(fwu->rmi4_data->irq);
		goto exit;
	}
	if (f01_device_status.flash_prog) {
		tp_log_warning("%s(line %d): In flash prog mode\n",
				__func__,__LINE__);
		fwu->in_flash_prog_mode = true;
	} else {
		fwu->in_flash_prog_mode = false;
	}

	if (fwu->do_lockdown) {
		switch (fwu->bl_version) {
		case V5:
		case V6:
			fwu->lockdown_data = fw_image + LOCKDOWN_OFFSET;
			fwu->lockdown_block_count = LOCKDOWN_BLOCK_COUNT;
			retval = fwu_do_lockdown();
			if (retval < 0) {
				tp_log_err("%s(line %d): Failed to do lockdown\n",
						__func__,__LINE__);
			}
		default:
			break;
		}
	}

	if (header.firmware_size)
		fwu->firmware_data = fw_image + FW_IMAGE_OFFSET;
	if (header.config_size) {
		fwu->config_data = fw_image + FW_IMAGE_OFFSET +
				header.firmware_size;
	}

	flash_area = fwu_go_nogo(&header);
	switch (flash_area) {
	case UI_FIRMWARE:
		retval = fwu_do_reflash();
		break;
	case CONFIG_AREA:
		retval = fwu_do_write_config();
		break;
	case NONE:
	default:
		enable_irq(fwu->rmi4_data->irq);
		goto exit;
	}

	if (retval < 0) {
		tp_log_err("%s(line %d): Failed to do reflash\n", __func__,__LINE__);
	}

	/* Move here to reduce time consumption when no need to upgrate fw */
	fwu->rmi4_data->reset_device(fwu->rmi4_data);
	retval = fwu_scan_pdt();
	tp_log_debug("%s(line %d):fwu_scan_pdt ,retval=%d",__func__,__LINE__,retval);
	if (retval < 0)	{
		tp_log_err("%s:scan_pdt fail\n", __func__);
	}

	/* free irq */
	retval = synaptics_rmi4_irq_enable(fwu->rmi4_data, false);
	if (retval < 0) {
		tp_log_err("%s(%d):Irq disable fail\n", __func__, __LINE__);
		goto exit;
	}
	mdelay(100);
	/* request irq */
	retval = synaptics_rmi4_irq_enable(fwu->rmi4_data, true);
	if (retval < 0) {
		tp_log_err("%s(%d):Irq disable fail\n", __func__, __LINE__);
		goto exit;
	}

exit:
	if (fw_entry)
		release_firmware(fw_entry);

	tp_log_warning("%s(line %d):End of reflash process, retval = %d\n",__func__,__LINE__,retval);

	fwu->rmi4_data->firmware_updating = false;
	pm_runtime_put(&fwu->rmi4_data->i2c_client->dev);

#ifdef CONFIG_HUAWEI_KERNEL
	synaptics_set_appinfo();
#endif /*CONFIG_HUAWEI_KERNEL*/

	return retval;
}
int synaptics_fw_upgrade(unsigned char *fw_data)
{
	int retval;

	tp_log_warning("%s(line %d): begin\n",__func__,__LINE__);
	if (!fwu) {
		tp_log_err("%s(line %d): fwu is null\n",__func__,__LINE__);
		return -ENODEV;
	}

	if (!fwu->initialized) {
		tp_log_err("%s(line %d): fwu is not init\n",__func__,__LINE__);
		return -ENODEV;
	}
	
	if (fwu->rmi4_data->board->esd_support) {
		synaptics_dsx_esd_suspend();
	}

	fwu->ext_data_source = fw_data;
	fwu->config_area = UI_CONFIG_AREA;

	// wake touch panel up
	pm_runtime_get_sync(&fwu->rmi4_data->i2c_client->dev);

	retval = fwu_start_reflash();

#ifdef CONFIG_HUAWEI_DSM
	/* if fw upgrade err, report err */
	if(retval<0) {
		synp_tp_report_dsm_err(DSM_TP_FW_ERROR_NO, retval);
	}
#endif/*CONFIG_HUAWEI_DSM*/

	pm_runtime_put(&fwu->rmi4_data->i2c_client->dev);


	if (fwu->rmi4_data->board->esd_support) {
		synaptics_dsx_esd_resume();
	}
	
	return retval;
}
EXPORT_SYMBOL(synaptics_fw_upgrade);

static void fwu_startup_fw_update_work(struct work_struct *work)
{
	tp_log_warning("%s(line %d): begin\n",__func__,__LINE__);
	synaptics_fw_upgrade(NULL);

	return;
}

static ssize_t fwu_sysfs_show_image(struct file *data_file,
		struct kobject *kobj, struct bin_attribute *attributes,
		char *buf, loff_t pos, size_t count)
{
	if (count < fwu->config_size) {
#ifdef CONFIG_64BIT
		tp_log_err("%s: Not enough space (%lu bytes) in buffer\n",
				__func__, count);
#else
		tp_log_err("%s: Not enough space (%d bytes) in buffer\n",
				__func__, count);
#endif
		return -EINVAL;
	}

	memcpy(buf, fwu->read_config_buf, fwu->config_size);

	return fwu->config_size;
}

static ssize_t fwu_sysfs_store_image(struct file *data_file,
		struct kobject *kobj, struct bin_attribute *attributes,
		char *buf, loff_t pos, size_t count)
{
	tp_log_info("%s(%d)store image data\n", __func__, __LINE__);
	memcpy((void *)(&fwu->ext_data_source[fwu->data_pos]),
			(const void *)buf,
			count);

	fwu->data_pos += count;

	return count;
}

static ssize_t fwu_sysfs_do_reflash_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned int input;

	tp_log_info("%s(%d)start manual update\n", __func__, __LINE__);
	if (sscanf(buf, "%u", &input) != 1) {
		retval = -EINVAL;
		goto exit;
	}

	if (input & LOCKDOWN) {
		fwu->do_lockdown = true;
		input &= ~LOCKDOWN;
	}

	if ((input != NORMAL) && (input != FORCE)) {
		retval = -EINVAL;
		goto exit;
	}

	if (input == FORCE)
		fwu->force_update = true;

	retval = synaptics_fw_upgrade(fwu->ext_data_source);
	if (retval < 0) {
		tp_log_err("%s: Failed to do reflash\n", __func__);
		goto exit;
	}

	retval = count;

exit:
	kfree(fwu->ext_data_source);
	fwu->ext_data_source = NULL;
	fwu->force_update = FORCE_UPDATE;
	fwu->do_lockdown = DO_LOCKDOWN;

	tp_log_info("%s(%d)start manual update finish\n", __func__, __LINE__);
	return retval;
}

static ssize_t fwu_sysfs_write_config_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned int input;

	if (sscanf(buf, "%u", &input) != 1) {
		retval = -EINVAL;
		goto exit;
	}

	if (input != 1) {
		retval = -EINVAL;
		goto exit;
	}

	retval = fwu_start_write_config();
	if (retval < 0) {
		tp_log_err("%s: Failed to write config\n",
				__func__);
		goto exit;
	}

	retval = count;

exit:
	kfree(fwu->ext_data_source);
	fwu->ext_data_source = NULL;
	return retval;
}

static ssize_t fwu_sysfs_read_config_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned int input;

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	if (input != 1)
		return -EINVAL;

	retval = fwu_do_read_config();
	if (retval < 0) {
		tp_log_err("%s: Failed to read config\n",
				__func__);
		return retval;
	}

	return count;
}

static ssize_t fwu_sysfs_config_area_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned long config_area;

	retval = sstrtoul(buf, 10, &config_area);
	if (retval)
		return retval;

	fwu->config_area = config_area;

	return count;
}

static ssize_t fwu_sysfs_image_name_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	memcpy(fwu->image_name, buf, count);

	return count;
}

static ssize_t fwu_sysfs_image_size_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned long size;

	retval = sstrtoul(buf, 10, &size);
	if (retval) {
		tp_log_err("%s(%d):parse file size fail, buf=%s, retval=%d\n", 
			__func__, __LINE__, buf, retval);
		return retval;
	}

	tp_log_info("%s(%d):firmware size:%ld\n", __func__, __LINE__, size);
	fwu->image_size = size;
	fwu->data_pos = 0;

	kfree(fwu->ext_data_source);
	fwu->ext_data_source = kzalloc(fwu->image_size, GFP_KERNEL);
	if (!fwu->ext_data_source) {
		tp_log_err("%s: Failed to alloc mem for image data\n",
				__func__);
		return -ENOMEM;
	}

	return count;
}

static ssize_t fwu_sysfs_hw_firmware_update_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	static atomic_t is_update = ATOMIC_INIT(0);
	char old_firmware_name[MAX_IMAGE_NAME_LEN] = {0};

	if (atomic_read(&is_update) == 1) {
		tp_log_err("%s %d:Firmware update is running\n", __func__, __LINE__);
		return -ENOMEM;
	} else {
		atomic_set(&is_update, 1);
	}

	// check firmware file name length
	if (strnlen(buf, PAGE_SIZE) >= MAX_IMAGE_NAME_LEN) {
		tp_log_err("%s %d:firmware name is to long, max length is:%d\n", 
			__func__, __LINE__, MAX_IMAGE_NAME_LEN - 1);
		return -ENOMEM;
	}

	strncpy(old_firmware_name, fwu->firmware_name, MAX_IMAGE_NAME_LEN);

	memset(fwu->firmware_name, 0, MAX_IMAGE_NAME_LEN);
	strncpy(fwu->firmware_name, buf, MAX_IMAGE_NAME_LEN - 1);

	// start firmware update
	synaptics_fw_upgrade(NULL);

	strncpy(fwu->firmware_name, old_firmware_name, MAX_IMAGE_NAME_LEN);

	atomic_set(&is_update, 0);

	return count;
}

static ssize_t fwu_sysfs_hw_firmware_update_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s",
		"help:\n" \
		"[1].put firmware file to /system/etc/firmware/\n" \
		"\tFor example:adb push ./synaptics.img /system/etc/firmware/ \n" \
		"[2].echo firmware name to hwFirmwareUpdate \n" \
		"\tFor example:echo synaptics.img > hwFirmwareUpdate\n");
}


static ssize_t fwu_sysfs_block_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->block_size);
}

static ssize_t fwu_sysfs_firmware_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->fw_block_count);
}

static ssize_t fwu_sysfs_configuration_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->config_block_count);
}

static ssize_t fwu_sysfs_perm_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->perm_config_block_count);
}

static ssize_t fwu_sysfs_bl_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->bl_config_block_count);
}

static ssize_t fwu_sysfs_disp_config_block_count_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fwu->disp_config_block_count);
}

static ssize_t fwu_sysfs_firmware_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int firmware_version_len = synaptics_get_firmware_version(buf, PAGE_SIZE - 1);
	strncat(buf, "\n", PAGE_SIZE - 1);
	firmware_version_len = strnlen(buf, PAGE_SIZE);
	return firmware_version_len;
}


static void synaptics_rmi4_fwu_attn(struct synaptics_rmi4_data *rmi4_data,
		unsigned char intr_mask)
{
	if(SYNAPTICS_S3718 == rmi4_data->board->ic_type) {
		synaptics_rmi4_fwu_s3718_attn(rmi4_data,intr_mask);
		return;
	}
	if (!fwu)
		return;

	if (fwu->intr_mask & intr_mask)
		fwu_read_f34_flash_status();

	return;
}

static int synaptics_get_firmware_version(char *config_id_ptr, unsigned long length) 
{
	int ret = 0;
	unsigned int config_id_ptr_len = 0;
	unsigned char config_id[4] = {0};

	// clear config id ptr
	memset(config_id_ptr, 0, length);
	
	/* Get device config ID */
	ret = fwu->fn_ptr->read(fwu->rmi4_data,	fwu->f34_fd.ctrl_base_addr,	config_id, sizeof(config_id));
	if (ret < 0) {
		tp_log_err("%s %d:read config id fail, ret=%d\n", __func__, __LINE__, ret);
		config_id_ptr_len = 0;
		goto exit;
	}
	
	tp_log_info("%s %d:Device config ID = 0x%02x 0x%02x 0x%02x 0x%02x\n",
			__func__,__LINE__,
			config_id[0],
			config_id[1],
			config_id[2],
			config_id[3]);

	// formate config id to string
	config_id_ptr_len = snprintf(config_id_ptr, length, "%x%02x", config_id[2], config_id[3]);
	tp_log_info("%s %d:config id:%s\n", __func__, __LINE__, config_id_ptr);

exit:
	return config_id_ptr_len;
}

static void fwu_get_fw_name(void)
{
	int retval = 0;
	unsigned char config_id[4] = {0xFF,0xFF,0xFF,0xFF};
	unsigned int device_config_id = 0;
	const char *module_name = NULL;
	const char *product_name = NULL;
	struct synaptics_rmi4_data *rmi4_data = fwu->rmi4_data;

	/* Get device config ID */
	retval = fwu->fn_ptr->read(fwu->rmi4_data,
				fwu->f34_fd.ctrl_base_addr,
				config_id,
				sizeof(config_id));
	if (retval < 0) {
		tp_log_err("%s %d:Failed to read device config ID\n", __func__,__LINE__);
		goto exit;
	}
	
	device_config_id = extract_uint_be(config_id);
	tp_log_debug("%s %d:Device config ID = 0x%02x 0x%02x 0x%02x 0x%02x\n",
			__func__,__LINE__,
			config_id[0],
			config_id[1],
			config_id[2],
			config_id[3]);
exit:
	/* Get device module name */
	module_name = get_cof_module_name(fwu->product_id);
	tp_log_warning("%s(line %d):module_name = %s \n", __func__,__LINE__, module_name);

	/* Use to get cap-test limit */
	memcpy(fwu->rmi4_data->product_id, fwu->product_id, SYNAPTICS_RMI4_PRODUCT_ID_SIZE);
	product_name = rmi4_data->board->product_name;
	strncat(fwu->firmware_name, product_name, strlen(product_name));
	strncat(fwu->firmware_name, "_", 1);
	strncat(fwu->firmware_name, module_name, strlen(module_name));
	strncat(fwu->firmware_name, "_fw.img", strlen("_fw.img"));
	tp_log_warning("%s %d:combine_fw_name = %s \n", __func__,__LINE__, fwu->firmware_name);

	return;
}

static int synaptics_rmi4_fwu_init(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
#ifdef CONFIG_APP_INFO
	int ret = 0;
#endif /*CONFIG_APP_INFO*/
	int attr_count;
	struct pdt_properties pdt_props;

	tp_log_warning("%s(line %d): begin\n",__func__,__LINE__);
	if (SYNAPTICS_S3718 == rmi4_data->board->ic_type) {
		tp_log_warning("%s(line %d): begin SYNAPTICS_S3718\n",__func__,__LINE__);
		retval=synap_update_work_init(rmi4_data);
		return retval;
	}
	tp_log_info("%s(line %d): begin SYNAPTICS_S3207\n",__func__,__LINE__);
	fwu = kzalloc(sizeof(*fwu), GFP_KERNEL);
	if (!fwu) {
		tp_log_err("%s: Failed to alloc mem for fwu\n",	__func__);
		retval = -ENOMEM;
		goto exit;
	}

	fwu->fn_ptr = kzalloc(sizeof(*(fwu->fn_ptr)), GFP_KERNEL);
	if (!fwu->fn_ptr) {
		tp_log_err("%s: Failed to alloc mem for fn_ptr\n", __func__);
		retval = -ENOMEM;
		goto exit_free_fwu;
	}

	fwu->image_name = kzalloc(MAX_IMAGE_NAME_LEN, GFP_KERNEL);
	if (!fwu->image_name) {
		tp_log_err("%s: Failed to alloc mem for image name\n", __func__);
		retval = -ENOMEM;
		goto exit_free_fn_ptr;
	}

	fwu->firmware_name = kzalloc(MAX_IMAGE_NAME_LEN, GFP_KERNEL);
	if (!fwu->firmware_name) {
		tp_log_err("%s: Failed to alloc mem for firmware_name_used\n", __func__);
		retval = -ENOMEM;
		goto exit_free_fn_ptr;
	}

	fwu->rmi4_data = rmi4_data;
	fwu->fn_ptr->read = rmi4_data->i2c_read;
	fwu->fn_ptr->write = rmi4_data->i2c_write;
	fwu->fn_ptr->enable = rmi4_data->irq_enable;

	retval = fwu->fn_ptr->read(rmi4_data,
			PDT_PROPS,
			pdt_props.data,
			sizeof(pdt_props.data));
	if (retval < 0) {
		tp_log_debug("%s: Failed to read PDT properties, assuming 0x00\n", __func__);
	} else if (pdt_props.has_bsr) {
		tp_log_err("%s: Reflash for LTS not currently supported\n", __func__);
		retval = -ENODEV;
#ifdef CONFIG_HUAWEI_DSM
		synp_tp_report_dsm_err( DSM_TP_PDT_PROPS_ERROR_NO, retval);
#endif/*CONFIG_HUAWEI_DSM*/
		goto exit_free_mem;
	}

	retval = fwu_scan_pdt();
	if (retval < 0) {
		tp_log_err("%s %d:scan pdt fail, retval=%d\n", __func__, __LINE__, retval);
		goto exit_free_mem;
	}

	fwu->productinfo1 = rmi4_data->rmi4_mod_info.product_info[0];
	fwu->productinfo2 = rmi4_data->rmi4_mod_info.product_info[1];
	memcpy(fwu->product_id, rmi4_data->rmi4_mod_info.product_id_string,
			SYNAPTICS_RMI4_PRODUCT_ID_SIZE);
	fwu->product_id[SYNAPTICS_RMI4_PRODUCT_ID_SIZE] = 0;

	tp_log_debug("%s: F01 product info: 0x%04x 0x%04x\n",
			__func__, fwu->productinfo1, fwu->productinfo2);
	tp_log_debug("%s: F01 product ID: %s\n",
			__func__, fwu->product_id);
	tp_log_warning("%s: F01 product ID: %s\n",
			__func__, fwu->product_id);

	fwu_get_fw_name();
	retval = fwu_read_f34_queries();
	if (retval < 0){
		tp_log_err("%s: failed to read f34 queries,retval = %d\n", __func__, retval);
#ifdef CONFIG_HUAWEI_DSM
		synp_tp_report_dsm_err( DSM_TP_F34_READ_QUERIES_ERROR_NO, retval);
#endif/*CONFIG_HUAWEI_DSM*/
		goto exit_free_mem;
	}

	fwu->force_update = FORCE_UPDATE;
	fwu->do_lockdown = DO_LOCKDOWN;
	fwu->initialized = true;

	retval = sysfs_create_bin_file(&rmi4_data->input_dev->dev.kobj,
			&dev_attr_data);
	if (retval < 0) {
		tp_log_err("%s: Failed to create sysfs bin file\n",	__func__);
		goto exit_free_mem;
	}

	for (attr_count = 0; attr_count < ARRAY_SIZE(attrs); attr_count++) {
		retval = sysfs_create_file(&rmi4_data->input_dev->dev.kobj,
				&attrs[attr_count].attr);
		if (retval < 0) {
			tp_log_err("%s: Failed to create sysfs attributes\n", __func__);
			retval = -ENODEV;
			goto exit_remove_attrs;
		}
	}

#ifdef DO_STARTUP_FW_UPDATE
	fwu->fwu_workqueue = create_singlethread_workqueue("fwu_workqueue");
	/*Solve the crash problem when fwu->fwu_workqueue is NULL*/
	if(NULL == fwu->fwu_workqueue){
		tp_log_err("%s: failed to create workqueue for fw upgrade\n", __func__);
		retval = -ENOMEM;
		goto exit_remove_attrs;
	}

	INIT_DELAYED_WORK(&fwu->fwu_work, fwu_startup_fw_update_work);
	queue_delayed_work(fwu->fwu_workqueue,
			&fwu->fwu_work,
			msecs_to_jiffies(STARTUP_FW_UPDATE_DELAY_MS));
#endif

	return 0;

exit_remove_attrs:
for (attr_count--; attr_count >= 0; attr_count--) {
	sysfs_remove_file(&rmi4_data->input_dev->dev.kobj,
			&attrs[attr_count].attr);
}

sysfs_remove_bin_file(&rmi4_data->input_dev->dev.kobj, &dev_attr_data);

exit_free_mem:
	kfree(fwu->image_name);
	kfree(fwu->firmware_name);

exit_free_fn_ptr:
	kfree(fwu->fn_ptr);

exit_free_fwu:
	kfree(fwu);
	fwu = NULL;

exit:
	/* In case that capci-test can not find app_info */
#ifdef CONFIG_APP_INFO
	ret = app_info_set("touch_panel", "synaptics_dsx_X.X");
	if (ret < 0)
	{
		tp_log_err("%s(line %d): app_info_set fail,ret=%d\n"
				,__func__,__LINE__,ret);
	}
#endif /*CONFIG_APP_INFO*/
	return retval;
}
static void synaptics_rmi4_fwu_remove(struct synaptics_rmi4_data *rmi4_data)
{
	unsigned char attr_count;

	if (!fwu)
		goto exit;

	sysfs_remove_bin_file(&rmi4_data->input_dev->dev.kobj, &dev_attr_data);

	for (attr_count = 0; attr_count < ARRAY_SIZE(attrs); attr_count++) {
		sysfs_remove_file(&rmi4_data->input_dev->dev.kobj,
				&attrs[attr_count].attr);
	}

	kfree(fwu->read_config_buf);
	kfree(fwu->image_name);
	kfree(fwu->firmware_name);
	kfree(fwu->fn_ptr);
	kfree(fwu);
	fwu = NULL;

exit:
	complete(&fwup_remove_complete);

	return;
}

static struct synaptics_rmi4_exp_fn fwu_module = {
	.fn_type = RMI_FW_UPDATER,
	.init = synaptics_rmi4_fwu_init,
	.remove = synaptics_rmi4_fwu_remove,
	.reset = NULL,
	.reinit = NULL,
	.early_suspend = NULL,
	.suspend = NULL,
	.resume = NULL,
	.late_resume = NULL,
	.attn = synaptics_rmi4_fwu_attn,
};

static int __init rmi4dsx_fw_update_module_init(void)
{
	tp_log_warning("%s(line %d): begin\n",__func__,__LINE__);
	synaptics_rmi4_new_func(&fwu_module, true);

	return 0;
}

static void __exit rmi4dsx_fw_update_module_exit(void)
{
	synaptics_rmi4_new_func(&fwu_module, false);

	wait_for_completion(&fwup_remove_complete);

	return;
}

module_init(rmi4dsx_fw_update_module_init);
module_exit(rmi4dsx_fw_update_module_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics DSX FW Update Module");
MODULE_LICENSE("GPL v2");

