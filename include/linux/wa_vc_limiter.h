/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Ardia-kun <ardia-kun@github.com>
 *
 * WhatsApp Video Call CPU Limiter (WAVC Limiter)
 * Automatically restricts CPU execution to little cores (CPU 0-5)
 * during WhatsApp video calls to prevent overheating and battery drain.
 */

#ifndef _LINUX_WA_VC_LIMITER_H
#define _LINUX_WA_VC_LIMITER_H

#include <linux/types.h>
#include <linux/sched.h>

#ifdef CONFIG_WA_VC_LIMITER

extern bool wavc_is_active(void);
extern int wavc_filter_target_cpu(struct task_struct *p, int cpu);
extern void wavc_notify_camera_state(bool active);
extern void wavc_notify_task_waking(struct task_struct *p);

#else /* !CONFIG_WA_VC_LIMITER */

static inline bool wavc_is_active(void)
{
	return false;
}

static inline int wavc_filter_target_cpu(struct task_struct *p, int cpu)
{
	return cpu;
}

static inline void wavc_notify_camera_state(bool active)
{
}

static inline void wavc_notify_task_waking(struct task_struct *p)
{
}

#endif /* CONFIG_WA_VC_LIMITER */

#endif /* _LINUX_WA_VC_LIMITER_H */
