/*
 * kernel/configs.c
 * Echo the kernel .config file used to build the kernel
 *
 * Copyright (C) 2002 Khalid Aziz <khalid_aziz@hp.com>
 * Copyright (C) 2002 Randy Dunlap <rdunlap@xenotime.net>
 * Copyright (C) 2002 Al Stone <ahs3@fc.hp.com>
 * Copyright (C) 2002 Hewlett-Packard Company
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/cred.h>
#include <linux/ratelimit.h>

/**************************************************/
/* the actual current config file                 */

/*
 * Define kernel_config_data and kernel_config_data_size, which contains the
 * wrapped and compressed configuration file.  The file is first compressed
 * with gzip and then bounded by two eight byte magic numbers to allow
 * extraction from a binary kernel image:
 *
 *   IKCFG_ST
 *   <image>
 *   IKCFG_ED
 */
#define MAGIC_START	"IKCFG_ST"
#define MAGIC_END	"IKCFG_ED"
#include "config_data_real.h"
#include "config_data_fake.h"

#define MAGIC_SIZE (sizeof(MAGIC_START) - 1)
#define kernel_config_data_real_size \
	(sizeof(kernel_config_data_real) - 1 - MAGIC_SIZE * 2)
#define kernel_config_data_fake_size \
	(sizeof(kernel_config_data_fake) - 1 - MAGIC_SIZE * 2)
#define kernel_config_data_size \
	(sizeof(kernel_config_data_real) > sizeof(kernel_config_data_fake) ? \
	 kernel_config_data_real_size : kernel_config_data_fake_size)

#ifdef CONFIG_IKCONFIG_PROC

static bool task_in_system_server_tree(struct task_struct *task)
{
	struct task_struct *t;

	rcu_read_lock();
	for (t = task; t && t != &init_task; t = t->real_parent) {
		if (!strcmp(t->comm, "system_server")) {
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();

	return false;
}

/*
 * Decide whether the current process should be served the fake (stock)
 * config instead of the real one.
 */
static bool config_should_spoof(void)
{
#ifdef CONFIG_IKCONFIG_SPOOF_FRAMEWORK
	/* The Android framework runs as AID_SYSTEM (uid 1000). */
	if (current_uid().val == 1000)
		return true;

	return task_in_system_server_tree(current);
#elif defined(CONFIG_IKCONFIG_SPOOF_SYSTEM_SERVER_TREE)
	return task_in_system_server_tree(current);
#elif defined(CONFIG_IKCONFIG_SPOOF_SYSTEM_SERVER)
	return !strcmp(current->comm, "system_server");
#else
	return false;
#endif
}

static ssize_t
ikconfig_read_current(struct file *file, char __user *buf,
		      size_t len, loff_t * offset)
{
	const char *data;
	size_t size;

	if (config_should_spoof()) {
		pr_info_ratelimited("config.gz: serving fake config to %s (pid %d, uid %d)\n",
				    current->comm, current->pid, current_uid().val);
		data = kernel_config_data_fake + MAGIC_SIZE;
		size = kernel_config_data_fake_size;
	} else {
		data = kernel_config_data_real + MAGIC_SIZE;
		size = kernel_config_data_real_size;
	}

	return simple_read_from_buffer(buf, len, offset, data, size);
}

static const struct file_operations ikconfig_file_ops = {
	.owner = THIS_MODULE,
	.read = ikconfig_read_current,
	.llseek = default_llseek,
};

static int __init ikconfig_init(void)
{
	struct proc_dir_entry *entry;

	/* create the current config file */
	entry = proc_create("config.gz", S_IFREG | S_IRUGO, NULL,
			    &ikconfig_file_ops);
	if (!entry)
		return -ENOMEM;

	proc_set_size(entry, kernel_config_data_size);

	return 0;
}

static void __exit ikconfig_cleanup(void)
{
	remove_proc_entry("config.gz", NULL);
}

module_init(ikconfig_init);
module_exit(ikconfig_cleanup);

#endif /* CONFIG_IKCONFIG_PROC */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Randy Dunlap");
MODULE_DESCRIPTION("Echo the kernel .config file used to build the kernel");
