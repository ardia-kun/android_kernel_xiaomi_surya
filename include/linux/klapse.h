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

#ifndef _LINUX_KLAPSE_H
#define _LINUX_KLAPSE_H

#include <linux/types.h>

#define KLAPSE_VERSION "5.0"

#ifdef CONFIG_KLAPSE
extern unsigned long klapse_red;
extern unsigned long klapse_green;
extern unsigned long klapse_blue;
extern unsigned int klapse_enabled;
extern void klapse_pulse(void);
#else
static inline void klapse_pulse(void) {}
#endif

#endif /* _LINUX_KLAPSE_H */
