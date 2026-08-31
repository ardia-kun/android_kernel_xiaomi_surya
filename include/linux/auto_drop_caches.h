/*
 * Auto Drop Caches (Auto Clear Cache) Driver
 *
 * Copyright (C) 2026 ardia-kun (kiddie@arch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _LINUX_AUTO_DROP_CACHES_H
#define _LINUX_AUTO_DROP_CACHES_H

#include <linux/types.h>

#define AUTO_DROP_CACHES_VERSION "1.0"

#ifdef CONFIG_AUTO_DROP_CACHES
extern unsigned int auto_drop_caches_enable;
extern void auto_drop_caches_trigger(void);
#else
static inline void auto_drop_caches_trigger(void) {}
#endif

#endif /* _LINUX_AUTO_DROP_CACHES_H */
