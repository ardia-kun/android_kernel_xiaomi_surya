/*
 * KLAPSE: Kernel-Level Automated Picture-adjustment & Solar Effect
 *
 * Copyright (C) 2018-2020 Tanmay (dev_harsh1998)
 * Copyright (C) 2026 ardia-kun
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/timekeeping.h>
#include <linux/time.h>
#include <linux/klapse.h>

#define KLAPSE_NAME "KLAPSE v5.0 by Tanmay"

unsigned int klapse_enabled = 0;
EXPORT_SYMBOL(klapse_enabled);

unsigned long klapse_red = 256;
EXPORT_SYMBOL(klapse_red);

unsigned long klapse_green = 256;
EXPORT_SYMBOL(klapse_green);

unsigned long klapse_blue = 256;
EXPORT_SYMBOL(klapse_blue);

static unsigned int daytime = 7;
static unsigned int target_temperature = 4500;
static unsigned int scaling_rate = 40;
static unsigned int dim_factor = 0;
static unsigned int pulse_freq = 60;

static struct timer_list klapse_timer;
static struct kobject *klapse_kobj;

void klapse_pulse(void)
{
	struct timespec64 ts;
	struct tm tm_val;
	unsigned int current_hour;

	if (!klapse_enabled)
		return;

	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec, 0, &tm_val);
	current_hour = tm_val.tm_hour;

	/* Time-based color adjustment (Mode 1) */
	if (klapse_enabled == 1) {
		if (current_hour >= 20 || current_hour < daytime) {
			/* Night profile: reduce blue light smoothly */
			klapse_red = 256;
			klapse_green = 230;
			klapse_blue = 180;
		} else if (current_hour >= 18 && current_hour < 20) {
			/* Sunset transition */
			klapse_red = 256;
			klapse_green = 245;
			klapse_blue = 215;
		} else {
			/* Daytime profile */
			klapse_red = 256;
			klapse_green = 256;
			klapse_blue = 256;
		}
	}
}
EXPORT_SYMBOL(klapse_pulse);

static void klapse_timer_callback(struct timer_list *t)
{
	klapse_pulse();
	mod_timer(&klapse_timer, jiffies + msecs_to_jiffies(pulse_freq * 1000));
}

/* Sysfs Interface */
static ssize_t klapse_name_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", KLAPSE_NAME);
}

static ssize_t enable_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", klapse_enabled);
}

static ssize_t enable_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	klapse_enabled = val;
	klapse_pulse();
	return count;
}

static ssize_t red_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", klapse_red);
}

static ssize_t red_store(struct kobject *kobj,
			 struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned long val;
	if (kstrtoul(buf, 10, &val) || val > 256)
		return -EINVAL;

	klapse_red = val;
	return count;
}

static ssize_t green_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", klapse_green);
}

static ssize_t green_store(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned long val;
	if (kstrtoul(buf, 10, &val) || val > 256)
		return -EINVAL;

	klapse_green = val;
	return count;
}

static ssize_t blue_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", klapse_blue);
}

static ssize_t blue_store(struct kobject *kobj,
			  struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	unsigned long val;
	if (kstrtoul(buf, 10, &val) || val > 256)
		return -EINVAL;

	klapse_blue = val;
	return count;
}

static ssize_t daytime_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", daytime);
}

static ssize_t daytime_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val > 23)
		return -EINVAL;

	daytime = val;
	klapse_pulse();
	return count;
}

static ssize_t target_temperature_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", target_temperature);
}

static ssize_t target_temperature_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val < 1000 || val > 10000)
		return -EINVAL;

	target_temperature = val;
	klapse_pulse();
	return count;
}

static ssize_t scaling_rate_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", scaling_rate);
}

static ssize_t scaling_rate_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	scaling_rate = val;
	return count;
}

static ssize_t dim_factor_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dim_factor);
}

static ssize_t dim_factor_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val > 100)
		return -EINVAL;

	dim_factor = val;
	return count;
}

static ssize_t pulse_freq_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", pulse_freq);
}

static ssize_t pulse_freq_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val < 1)
		return -EINVAL;

	pulse_freq = val;
	return count;
}

static struct kobj_attribute klapse_name_attr = __ATTR_RO(klapse_name);
static struct kobj_attribute enable_attr = __ATTR_RW(enable);
static struct kobj_attribute red_attr = __ATTR_RW(red);
static struct kobj_attribute green_attr = __ATTR_RW(green);
static struct kobj_attribute blue_attr = __ATTR_RW(blue);
static struct kobj_attribute daytime_attr = __ATTR_RW(daytime);
static struct kobj_attribute target_temperature_attr = __ATTR_RW(target_temperature);
static struct kobj_attribute scaling_rate_attr = __ATTR_RW(scaling_rate);
static struct kobj_attribute dim_factor_attr = __ATTR_RW(dim_factor);
static struct kobj_attribute pulse_freq_attr = __ATTR_RW(pulse_freq);

static struct attribute *klapse_attrs[] = {
	&klapse_name_attr.attr,
	&enable_attr.attr,
	&red_attr.attr,
	&green_attr.attr,
	&blue_attr.attr,
	&daytime_attr.attr,
	&target_temperature_attr.attr,
	&scaling_rate_attr.attr,
	&dim_factor_attr.attr,
	&pulse_freq_attr.attr,
	NULL,
};

static struct attribute_group klapse_attr_group = {
	.attrs = klapse_attrs,
};

static int __init klapse_init(void)
{
	int rc;

	klapse_kobj = kobject_create_and_add("klapse", NULL);
	if (!klapse_kobj)
		return -ENOMEM;

	rc = sysfs_create_group(klapse_kobj, &klapse_attr_group);
	if (rc) {
		kobject_put(klapse_kobj);
		return rc;
	}

	timer_setup(&klapse_timer, klapse_timer_callback, 0);
	mod_timer(&klapse_timer, jiffies + msecs_to_jiffies(pulse_freq * 1000));

	pr_info("%s initialized successfully\n", KLAPSE_NAME);
	return 0;
}

static void __exit klapse_exit(void)
{
	del_timer_sync(&klapse_timer);
	if (klapse_kobj) {
		sysfs_remove_group(klapse_kobj, &klapse_attr_group);
		kobject_put(klapse_kobj);
	}
}

module_init(klapse_init);
module_exit(klapse_exit);
MODULE_DESCRIPTION("KLAPSE: Kernel-Level Automated Picture-adjustment & Solar Effect");
MODULE_LICENSE("GPL v2");
