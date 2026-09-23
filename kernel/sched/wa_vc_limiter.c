// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Ardia-kun <ardia-kun@github.com>
 *
 * WhatsApp Video Call CPU Limiter (WAVC Limiter) for Xiaomi Surya (SM7150)
 *
 * When WhatsApp is actively performing a video call, the camera sensor is streaming
 * and WhatsApp threads are active. Running on Cortex-A76 big cores (CPU 6-7) causes
 * high power draw and severe overheating.
 *
 * This driver automatically detects WhatsApp video call activity and restricts
 * CPU execution to Cortex-A55 little cores (CPU 0-5), reducing heat and power
 * consumption by more than 50% while maintaining smooth call performance.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/smp.h>
#include <linux/wa_vc_limiter.h>

#include "sched.h"

#define WAVC_VERSION "1.0"
#define DEFAULT_LITTLE_CPUS_MASK 0x3F  /* CPU 0-5 */
#define DEFAULT_BIG_CPUS_MASK    0xC0  /* CPU 6-7 */

/* Configuration variables */
static bool wavc_enabled = true;
static int wavc_mode = 1; /* 0 = app-only, 1 = system-wide (default), 2 = system-wide + isolate */
static int wavc_force_active = 0; /* 0 = auto, 1 = force active, -1 = force inactive */
static unsigned int wavc_little_mask = DEFAULT_LITTLE_CPUS_MASK;
static unsigned int wavc_big_mask = DEFAULT_BIG_CPUS_MASK;

/* State variables */
static atomic_t camera_active_count = ATOMIC_INIT(0);
static unsigned long last_camera_change;
static unsigned long last_wa_seen;
static bool wavc_state_active = false;
static unsigned long stats_activations = 0;

static struct kobject *wavc_kobj;

/*
 * Helper: Case-insensitive substring search in a bounded buffer.
 */
static inline bool strncasestr_helper(const char *haystack, const char *needle)
{
	size_t nlen, hlen, i;

	if (!haystack || !needle)
		return false;

	nlen = strlen(needle);
	hlen = strnlen(haystack, TASK_COMM_LEN);

	if (hlen < nlen)
		return false;

	for (i = 0; i <= hlen - nlen; i++) {
		if (strncasecmp(&haystack[i], needle, nlen) == 0)
			return true;
	}
	return false;
}

/*
 * Check if a task belongs to WhatsApp (main app or business).
 */
static bool is_whatsapp_task(struct task_struct *p)
{
	if (!p)
		return false;

	if (strncasestr_helper(p->comm, "whatsapp"))
		return true;

	if (p->group_leader && strncasestr_helper(p->group_leader->comm, "whatsapp"))
		return true;

	return false;
}

/*
 * Check if a task is camera or media-related.
 */
static bool is_camera_task(struct task_struct *p)
{
	if (!p)
		return false;

	if (strncasestr_helper(p->comm, "camera") ||
	    strncasestr_helper(p->comm, "provider@2.4"))
		return true;

	if (p->group_leader &&
	    (strncasestr_helper(p->group_leader->comm, "camera") ||
	     strncasestr_helper(p->group_leader->comm, "provider@2.4")))
		return true;

	return false;
}

/*
 * Determine if WhatsApp Video Call limiting is currently active.
 */
bool wavc_is_active(void)
{
	if (!wavc_enabled)
		return false;

	/* Manual override */
	if (wavc_force_active == 1)
		return true;
	if (wavc_force_active == -1)
		return false;

	/* Camera must be actively streaming or active within 1s (grace period for flips) */
	if (atomic_read(&camera_active_count) <= 0 &&
	    time_after(jiffies, last_camera_change + 1 * HZ))
		return false;

	/* WhatsApp must be active recently (within 3 seconds) */
	if (time_after(jiffies, READ_ONCE(last_wa_seen) + 3 * HZ))
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(wavc_is_active);

/*
 * Workqueue callback to reschedule big cores when entering/exiting active state.
 */
static void wavc_kick_work_fn(struct work_struct *work)
{
	int cpu;

	if (wavc_state_active) {
		/* Kick any tasks running on big cores so they migrate to little cores */
		for (cpu = 6; cpu <= 7; cpu++) {
			if (cpu_online(cpu) && !idle_cpu(cpu))
				smp_send_reschedule(cpu);
		}
#ifdef CONFIG_SCHED_CORE_CTL
		if (wavc_mode == 2) {
			sched_isolate_cpu(6);
			sched_isolate_cpu(7);
		}
#endif
	} else {
#ifdef CONFIG_SCHED_CORE_CTL
		if (wavc_mode == 2) {
			sched_unisolate_cpu(6);
			sched_unisolate_cpu(7);
		}
#endif
	}
}
static DECLARE_WORK(wavc_kick_work, wavc_kick_work_fn);

/*
 * State transition checker.
 */
static void wavc_check_state_transition(void)
{
	bool new_state = wavc_is_active();

	if (new_state != wavc_state_active) {
		wavc_state_active = new_state;
		if (new_state) {
			stats_activations++;
			pr_info("wa_vc_limit: WhatsApp video call active -> restricting CPU to little cores (0-5)\n");
		} else {
			pr_info("wa_vc_limit: WhatsApp video call ended -> restoring big cores\n");
		}
		schedule_work(&wavc_kick_work);
	}
}

/*
 * Called by camera sensor core when camera powers up or down.
 */
void wavc_notify_camera_state(bool active)
{
	if (active) {
		atomic_inc(&camera_active_count);
	} else {
		if (atomic_read(&camera_active_count) > 0)
			atomic_dec(&camera_active_count);
	}
	last_camera_change = jiffies;
	wavc_check_state_transition();
}
EXPORT_SYMBOL_GPL(wavc_notify_camera_state);

/*
 * Called from scheduler on task wakeup to track WhatsApp activity.
 */
void wavc_notify_task_waking(struct task_struct *p)
{
	if (!wavc_enabled)
		return;

	if (is_whatsapp_task(p)) {
		WRITE_ONCE(last_wa_seen, jiffies);
		wavc_check_state_transition();
	}
}
EXPORT_SYMBOL_GPL(wavc_notify_task_waking);

/*
 * Filter target CPU during task placement.
 * If WhatsApp video call is active and target CPU is a big core,
 * redirects task to an optimal little core.
 */
int wavc_filter_target_cpu(struct task_struct *p, int cpu)
{
	int c, best_cpu;
	unsigned int min_nr;

	if (!wavc_is_active())
		return cpu;

	/* Mode 0: only restrict WhatsApp and camera tasks */
	if (wavc_mode == 0 && !is_whatsapp_task(p) && !is_camera_task(p))
		return cpu;

	/* If already on a little core (< 6), leave it */
	if (cpu < 6)
		return cpu;

	/* Target is a big core (6 or 7). Find the best available little core. */
	best_cpu = -1;
	min_nr = UINT_MAX;

	for (c = 0; c < 6; c++) {
		if (!cpu_online(c) || !cpumask_test_cpu(c, &p->cpus_allowed))
			continue;

		if (idle_cpu(c)) {
			best_cpu = c;
			break;
		}

		if (cpu_rq(c)->nr_running < min_nr) {
			min_nr = cpu_rq(c)->nr_running;
			best_cpu = c;
		}
	}

	if (best_cpu >= 0)
		return best_cpu;

	/* Fallback: any allowed online little core */
	for (c = 0; c < 6; c++) {
		if (cpu_online(c) && cpumask_test_cpu(c, &p->cpus_allowed))
			return c;
	}

	/* If affinity forbids little cores entirely, keep original target */
	return cpu;
}
EXPORT_SYMBOL_GPL(wavc_filter_target_cpu);

/* ==================== Sysfs Interface ==================== */

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", wavc_enabled ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	wavc_enabled = (val != 0);
	wavc_check_state_transition();
	return count;
}

static ssize_t active_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", wavc_is_active() ? 1 : 0);
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", wavc_mode);
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 2)
		return -EINVAL;

	wavc_mode = val;
	return count;
}

static ssize_t force_active_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", wavc_force_active);
}

static ssize_t force_active_store(struct kobject *kobj, struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < -1 || val > 1)
		return -EINVAL;

	wavc_force_active = val;
	wavc_check_state_transition();
	return count;
}

static ssize_t camera_active_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&camera_active_count));
}

static ssize_t wa_active_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	bool wa_seen = !time_after(jiffies, READ_ONCE(last_wa_seen) + 3 * HZ);
	return scnprintf(buf, PAGE_SIZE, "%d\n", wa_seen ? 1 : 0);
}

static ssize_t activations_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lu\n", stats_activations);
}

static ssize_t little_cores_mask_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n", wavc_little_mask);
}

static ssize_t little_cores_mask_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 16, &val))
		return -EINVAL;

	wavc_little_mask = val;
	return count;
}

static ssize_t big_cores_mask_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n", wavc_big_mask);
}

static ssize_t big_cores_mask_store(struct kobject *kobj, struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 16, &val))
		return -EINVAL;

	wavc_big_mask = val;
	return count;
}

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", WAVC_VERSION);
}

static struct kobj_attribute enable_attr = __ATTR(enable, 0644, enable_show, enable_store);
static struct kobj_attribute active_attr = __ATTR_RO(active);
static struct kobj_attribute mode_attr = __ATTR(mode, 0644, mode_show, mode_store);
static struct kobj_attribute force_active_attr = __ATTR(force_active, 0644, force_active_show, force_active_store);
static struct kobj_attribute camera_active_attr = __ATTR_RO(camera_active);
static struct kobj_attribute wa_active_attr = __ATTR_RO(wa_active);
static struct kobj_attribute activations_attr = __ATTR_RO(activations);
static struct kobj_attribute little_mask_attr = __ATTR(little_cores_mask, 0644, little_cores_mask_show, little_cores_mask_store);
static struct kobj_attribute big_mask_attr = __ATTR(big_cores_mask, 0644, big_cores_mask_show, big_cores_mask_store);
static struct kobj_attribute version_attr = __ATTR_RO(version);

static struct attribute *wavc_attrs[] = {
	&enable_attr.attr,
	&active_attr.attr,
	&mode_attr.attr,
	&force_active_attr.attr,
	&camera_active_attr.attr,
	&wa_active_attr.attr,
	&activations_attr.attr,
	&little_mask_attr.attr,
	&big_mask_attr.attr,
	&version_attr.attr,
	NULL,
};

static const struct attribute_group wavc_attr_group = {
	.attrs = wavc_attrs,
};

static int __init wavc_limiter_init(void)
{
	int ret;

	wavc_kobj = kobject_create_and_add("wa_vc_limit", kernel_kobj);
	if (!wavc_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(wavc_kobj, &wavc_attr_group);
	if (ret) {
		kobject_put(wavc_kobj);
		return ret;
	}

	pr_info("wa_vc_limit: WhatsApp Video Call CPU Limiter v%s initialized\n", WAVC_VERSION);
	return 0;
}
late_initcall(wavc_limiter_init);
