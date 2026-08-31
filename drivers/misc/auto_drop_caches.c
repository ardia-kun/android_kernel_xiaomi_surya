/*
 * Auto Drop Caches (Auto Clear Cache) Driver
 * Automatically frees pagecache and slab dentries/inodes during screen off or periodically
 *
 * Copyright (C) 2026 ardia-kun (kiddie@arch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/jiffies.h>
#include <linux/auto_drop_caches.h>
#include <linux/msm_drm_notify.h>

#define DRIVER_NAME "auto_drop_caches"

unsigned int auto_drop_caches_enable = 1;
EXPORT_SYMBOL(auto_drop_caches_enable);

static unsigned int drop_level = 3; /* 1: pagecache, 2: slab, 3: both */
static unsigned int screen_off_delay_sec = 5; /* wait 5s after screen off */
static unsigned int timer_interval_min = 60; /* 60 minutes for periodic mode */
static unsigned long clear_count = 0;
static bool screen_is_on = true;

static struct workqueue_struct *adc_wq;
static struct delayed_work adc_delayed_work;
static struct timer_list adc_periodic_timer;
static struct kobject *adc_kobj;

static void adc_work_fn(struct work_struct *work)
{
	if (!auto_drop_caches_enable)
		return;

	/* If in screen-off mode and screen woke back up during delay, cancel */
	if ((auto_drop_caches_enable == 1 || auto_drop_caches_enable == 3) && screen_is_on)
		return;

	mm_drop_caches(drop_level);
	clear_count++;
	pr_debug("Auto Drop Caches: Cleared caches (level %u, total %lu)\n",
		 drop_level, clear_count);
}

void auto_drop_caches_trigger(void)
{
	if (adc_wq)
		queue_delayed_work(adc_wq, &adc_delayed_work, 0);
}
EXPORT_SYMBOL(auto_drop_caches_trigger);

static void adc_periodic_timer_callback(struct timer_list *t)
{
	if (auto_drop_caches_enable == 2 || auto_drop_caches_enable == 3) {
		if (adc_wq)
			queue_delayed_work(adc_wq, &adc_delayed_work, 0);
	}

	if (timer_interval_min > 0)
		mod_timer(&adc_periodic_timer,
			  jiffies + msecs_to_jiffies(timer_interval_min * 60 * 1000));
}

static int adc_drm_notifier_callback(struct notifier_block *self,
				     unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;

	if (!evdata || (evdata->id != MSM_DRM_PRIMARY_DISPLAY))
		return 0;

	if (event == MSM_DRM_EVENT_BLANK) {
		blank = evdata->data;
		if (blank) {
			if (*blank == MSM_DRM_BLANK_UNBLANK) {
				screen_is_on = true;
				if (adc_wq)
					cancel_delayed_work(&adc_delayed_work);
			} else if (*blank == MSM_DRM_BLANK_POWERDOWN) {
				screen_is_on = false;
				if (auto_drop_caches_enable == 1 || auto_drop_caches_enable == 3) {
					if (adc_wq)
						queue_delayed_work(adc_wq, &adc_delayed_work,
								   msecs_to_jiffies(screen_off_delay_sec * 1000));
				}
			}
		}
	}
	return 0;
}

static struct notifier_block adc_drm_notif = {
	.notifier_call = adc_drm_notifier_callback,
};

static int adc_fb_notifier_callback(struct notifier_block *self,
				    unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			screen_is_on = true;
			if (adc_wq)
				cancel_delayed_work(&adc_delayed_work);
		} else if (*blank == FB_BLANK_POWERDOWN) {
			screen_is_on = false;
			if (auto_drop_caches_enable == 1 || auto_drop_caches_enable == 3) {
				if (adc_wq)
					queue_delayed_work(adc_wq, &adc_delayed_work,
							   msecs_to_jiffies(screen_off_delay_sec * 1000));
			}
		}
	}
	return 0;
}

static struct notifier_block adc_fb_notif = {
	.notifier_call = adc_fb_notifier_callback,
};

/* Sysfs Controls */
static ssize_t enable_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", auto_drop_caches_enable);
}

static ssize_t enable_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val > 3)
		return -EINVAL;

	auto_drop_caches_enable = val;
	return count;
}

static ssize_t drop_level_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", drop_level);
}

static ssize_t drop_level_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val < 1 || val > 3)
		return -EINVAL;

	drop_level = val;
	return count;
}

static ssize_t screen_off_delay_sec_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", screen_off_delay_sec);
}

static ssize_t screen_off_delay_sec_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val > 3600)
		return -EINVAL;

	screen_off_delay_sec = val;
	return count;
}

static ssize_t timer_interval_min_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", timer_interval_min);
}

static ssize_t timer_interval_min_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int val;
	if (kstrtouint(buf, 10, &val) || val < 1 || val > 1440)
		return -EINVAL;

	timer_interval_min = val;
	mod_timer(&adc_periodic_timer,
		  jiffies + msecs_to_jiffies(timer_interval_min * 60 * 1000));
	return count;
}

static ssize_t clear_count_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", clear_count);
}

static ssize_t clear_now_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	mm_drop_caches(drop_level);
	clear_count++;
	return count;
}

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", AUTO_DROP_CACHES_VERSION);
}

static struct kobj_attribute enable_attr = __ATTR_RW(enable);
static struct kobj_attribute drop_level_attr = __ATTR_RW(drop_level);
static struct kobj_attribute screen_off_delay_sec_attr = __ATTR_RW(screen_off_delay_sec);
static struct kobj_attribute timer_interval_min_attr = __ATTR_RW(timer_interval_min);
static struct kobj_attribute clear_count_attr = __ATTR_RO(clear_count);
static struct kobj_attribute clear_now_attr = __ATTR_WO(clear_now);
static struct kobj_attribute version_attr = __ATTR_RO(version);

static struct attribute *adc_attrs[] = {
	&enable_attr.attr,
	&drop_level_attr.attr,
	&screen_off_delay_sec_attr.attr,
	&timer_interval_min_attr.attr,
	&clear_count_attr.attr,
	&clear_now_attr.attr,
	&version_attr.attr,
	NULL,
};

static struct attribute_group adc_attr_group = {
	.attrs = adc_attrs,
};

static int __init auto_drop_caches_init(void)
{
	int rc;

	adc_wq = alloc_ordered_workqueue("adc_wq", WQ_MEM_RECLAIM);
	if (!adc_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&adc_delayed_work, adc_work_fn);

	adc_kobj = kobject_create_and_add("auto_drop_caches", kernel_kobj);
	if (!adc_kobj) {
		destroy_workqueue(adc_wq);
		return -ENOMEM;
	}

	rc = sysfs_create_group(adc_kobj, &adc_attr_group);
	if (rc) {
		kobject_put(adc_kobj);
		destroy_workqueue(adc_wq);
		return rc;
	}

	timer_setup(&adc_periodic_timer, adc_periodic_timer_callback, 0);
	mod_timer(&adc_periodic_timer,
		  jiffies + msecs_to_jiffies(timer_interval_min * 60 * 1000));

	msm_drm_register_client(&adc_drm_notif);
	fb_register_client(&adc_fb_notif);

	pr_info("Auto Drop Caches: Version %s initialized successfully\n",
		AUTO_DROP_CACHES_VERSION);
	return 0;
}

static void __exit auto_drop_caches_exit(void)
{
	del_timer_sync(&adc_periodic_timer);
	if (adc_wq) {
		cancel_delayed_work_sync(&adc_delayed_work);
		destroy_workqueue(adc_wq);
	}
	if (adc_kobj) {
		sysfs_remove_group(adc_kobj, &adc_attr_group);
		kobject_put(adc_kobj);
	}
}

module_init(auto_drop_caches_init);
module_exit(auto_drop_caches_exit);
MODULE_DESCRIPTION("Auto Drop Caches (Auto Clear Cache) Driver");
MODULE_LICENSE("GPL v2");
