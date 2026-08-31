/*
 * Dynamic Fsync
 *
 * Copyright (C) 2013 Paul Reioux (Faux123)
 * Copyright (C) 2014-2021 Flar2 (Aaron Kling)
 * Copyright (C) 2026 ardia-kun
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/dynamic_fsync.h>
#include <linux/msm_drm_notify.h>

bool dyn_fsync_active = true;
EXPORT_SYMBOL(dyn_fsync_active);

static bool dyn_fsync_suspended = false;
static struct workqueue_struct *dyn_fsync_wq;
static struct work_struct dyn_fsync_flush_work;

bool dyn_fsync_is_suspended(void)
{
	return dyn_fsync_suspended;
}
EXPORT_SYMBOL(dyn_fsync_is_suspended);

static void dyn_fsync_flush_fn(struct work_struct *work)
{
	/* Flush all dirty buffers to storage when screen goes off */
	emergency_sync();
}

static int dyn_fsync_drm_notifier_callback(struct notifier_block *self,
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
				dyn_fsync_suspended = false;
			} else if (*blank == MSM_DRM_BLANK_POWERDOWN) {
				dyn_fsync_suspended = true;
				if (dyn_fsync_active && dyn_fsync_wq)
					queue_work(dyn_fsync_wq, &dyn_fsync_flush_work);
			}
		}
	}
	return 0;
}

static struct notifier_block dyn_fsync_drm_notif = {
	.notifier_call = dyn_fsync_drm_notifier_callback,
};

static int dyn_fsync_fb_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			dyn_fsync_suspended = false;
		} else if (*blank == FB_BLANK_POWERDOWN) {
			dyn_fsync_suspended = true;
			if (dyn_fsync_active && dyn_fsync_wq)
				queue_work(dyn_fsync_wq, &dyn_fsync_flush_work);
		}
	}
	return 0;
}

static struct notifier_block dyn_fsync_fb_notif = {
	.notifier_call = dyn_fsync_fb_notifier_callback,
};

/* Sysfs interface */
static ssize_t dyn_fsync_active_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dyn_fsync_active ? 1 : 0);
}

static ssize_t dyn_fsync_active_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	dyn_fsync_active = !!val;
	if (!dyn_fsync_active && dyn_fsync_wq)
		queue_work(dyn_fsync_wq, &dyn_fsync_flush_work);

	return count;
}

static ssize_t dyn_fsync_version_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", DYN_FSYNC_VERSION);
}

static struct kobj_attribute dyn_fsync_active_attr =
	__ATTR(Dyn_fsync_active, 0644, dyn_fsync_active_show, dyn_fsync_active_store);

static struct kobj_attribute dyn_fsync_version_attr =
	__ATTR(version, 0444, dyn_fsync_version_show, NULL);

static struct attribute *dyn_fsync_attrs[] = {
	&dyn_fsync_active_attr.attr,
	&dyn_fsync_version_attr.attr,
	NULL,
};

static struct attribute_group dyn_fsync_attr_group = {
	.attrs = dyn_fsync_attrs,
};

static struct kobject *dyn_fsync_kobj;

static int __init dynamic_fsync_init(void)
{
	int rc;

	dyn_fsync_wq = alloc_ordered_workqueue("dyn_fsync_wq", WQ_MEM_RECLAIM);
	if (!dyn_fsync_wq)
		return -ENOMEM;

	INIT_WORK(&dyn_fsync_flush_work, dyn_fsync_flush_fn);

	dyn_fsync_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);
	if (!dyn_fsync_kobj) {
		destroy_workqueue(dyn_fsync_wq);
		return -ENOMEM;
	}

	rc = sysfs_create_group(dyn_fsync_kobj, &dyn_fsync_attr_group);
	if (rc) {
		kobject_put(dyn_fsync_kobj);
		destroy_workqueue(dyn_fsync_wq);
		return rc;
	}

	msm_drm_register_client(&dyn_fsync_drm_notif);
	fb_register_client(&dyn_fsync_fb_notif);

	pr_info("Dynamic Fsync: Version %s initialized\n", DYN_FSYNC_VERSION);
	return 0;
}

static void __exit dynamic_fsync_exit(void)
{
	if (dyn_fsync_kobj) {
		sysfs_remove_group(dyn_fsync_kobj, &dyn_fsync_attr_group);
		kobject_put(dyn_fsync_kobj);
	}
	if (dyn_fsync_wq) {
		cancel_work_sync(&dyn_fsync_flush_work);
		destroy_workqueue(dyn_fsync_wq);
	}
}

module_init(dynamic_fsync_init);
module_exit(dynamic_fsync_exit);
MODULE_DESCRIPTION("Dynamic Fsync Driver");
MODULE_LICENSE("GPL v2");
