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

#ifndef _LINUX_DYNAMIC_FSYNC_H
#define _LINUX_DYNAMIC_FSYNC_H

#include <linux/types.h>

#define DYN_FSYNC_VERSION "2.0"

#ifdef CONFIG_DYNAMIC_FSYNC
extern bool dyn_fsync_active;
extern bool dyn_fsync_is_suspended(void);
#else
static const bool dyn_fsync_active = false;
static inline bool dyn_fsync_is_suspended(void) { return true; }
#endif

#endif /* _LINUX_DYNAMIC_FSYNC_H */
