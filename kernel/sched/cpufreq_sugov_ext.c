// SPDX-License-Identifier: GPL-2.0
/*
 * Schedutil Extended (sugov_ext) CPUFreq governor based on scheduler utilization data.
 *
 * Designed for low-latency responsiveness, power efficiency, and extended tunables
 * on heterogeneous multi-core architectures (Qualcomm Snapdragon / EAS / PELT / WALT).
 */

#define pr_fmt(fmt) "sugov_ext: " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include "sched.h"

#define SUGOV_EXT_KTHREAD_PRIORITY	50

struct sugov_ext_tunables {
	struct gov_attr_set attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int		hispeed_load;
	unsigned int		hispeed_freq;
	unsigned int		freq_margin_load;
	bool			boost;
	bool			pl;
	bool			iowait_boost_enable;
	bool			fast_ramp_down;
};

struct sugov_ext_policy {
	struct cpufreq_policy *policy;

	struct sugov_ext_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	s64			freq_update_delay_ns;
	u64 last_ws;
	u64 curr_cycles;
	u64 last_cyc_update_time;
	unsigned long avg_cap;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	unsigned long hispeed_util;
	unsigned long max;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
};

struct sugov_ext_cpu {
	struct update_util_data update_util;
	struct sugov_ext_policy *sg_policy;
	unsigned int cpu;

	bool iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_boost_max;
	u64 last_update;

	struct sched_walt_cpu_load walt_load;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_ext_cpu, sugov_ext_cpu);
static unsigned int sugov_ext_stale_ns;
static DEFINE_PER_CPU(struct sugov_ext_tunables *, sugov_ext_cached_tunables);

/************************ Governor internals ***********************/

static bool sugov_ext_should_update_freq(struct sugov_ext_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-cpu data is fully serialized.
	 */
	if (sg_policy->policy->fast_switch_enabled &&
	    !cpufreq_can_do_remote_dvfs(sg_policy->policy))
		return false;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool sugov_ext_up_down_rate_limit(struct sugov_ext_policy *sg_policy, u64 time,
					 unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - sg_policy->last_freq_update_time;

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
		return true;

	if (next_freq < sg_policy->next_freq) {
		if (sg_policy->tunables->fast_ramp_down &&
		    next_freq <= sg_policy->policy->min)
			return false;

		if (delta_ns < sg_policy->down_rate_delay_ns)
			return true;
	}

	return false;
}

static inline bool sugov_ext_use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return (!sysctl_sched_use_walt_cpu_util || walt_disabled);
#else
	return true;
#endif
}

static unsigned long sugov_ext_freq_to_util(struct sugov_ext_policy *sg_policy,
					    unsigned int freq)
{
	return mult_frac(sg_policy->max, freq,
			 sg_policy->policy->cpuinfo.max_freq);
}

#define KHZ 1000
static void sugov_ext_track_cycles(struct sugov_ext_policy *sg_policy,
				   unsigned int prev_freq,
				   u64 upto)
{
	u64 delta_ns, cycles;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	/* Track cycles in current window */
	delta_ns = upto - sg_policy->last_cyc_update_time;
	delta_ns *= prev_freq;
	do_div(delta_ns, (NSEC_PER_SEC / KHZ));
	cycles = delta_ns;
	sg_policy->curr_cycles += cycles;
	sg_policy->last_cyc_update_time = upto;
}

static void sugov_ext_calc_avg_cap(struct sugov_ext_policy *sg_policy, u64 curr_ws,
				   unsigned int prev_freq)
{
	u64 last_ws = sg_policy->last_ws;
	unsigned int avg_freq;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	BUG_ON(curr_ws < last_ws);
	if (curr_ws <= last_ws)
		return;

	/* If we skipped some windows */
	if (curr_ws > (last_ws + sched_ravg_window)) {
		avg_freq = prev_freq;
		/* Reset tracking history */
		sg_policy->last_cyc_update_time = curr_ws;
	} else {
		sugov_ext_track_cycles(sg_policy, prev_freq, curr_ws);
		avg_freq = sg_policy->curr_cycles;
		avg_freq /= sched_ravg_window / (NSEC_PER_SEC / KHZ);
	}
	sg_policy->avg_cap = sugov_ext_freq_to_util(sg_policy, avg_freq);
	sg_policy->curr_cycles = 0;
	sg_policy->last_ws = curr_ws;
}

static void sugov_ext_update_commit(struct sugov_ext_policy *sg_policy, u64 time,
				    unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int cpu;

	if (sg_policy->next_freq == next_freq)
		return;

	if (sugov_ext_up_down_rate_limit(sg_policy, time, next_freq)) {
		sg_policy->cached_raw_freq = 0;
		return;
	}

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	if (policy->fast_switch_enabled) {
		sugov_ext_track_cycles(sg_policy, sg_policy->policy->cur, time);
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (!next_freq)
			return;

		policy->cur = next_freq;
		for_each_cpu(cpu, policy->cpus) {
			trace_cpu_frequency(next_freq, cpu);
		}
	} else {
		if (sugov_ext_use_pelt())
			sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

#define DEFAULT_TARGET_LOAD 80

/**
 * sugov_ext_get_next_freq - Compute new frequency for a given cpufreq policy
 * @sg_policy: sugov_ext policy object
 * @util: Current CPU utilization
 * @max: CPU capacity
 */
static unsigned int sugov_ext_get_next_freq(struct sugov_ext_policy *sg_policy,
					    unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;
	unsigned int target_load = sg_policy->tunables->freq_margin_load;

	if (unlikely(sg_policy->tunables->boost))
		return policy->cpuinfo.max_freq;

	if (target_load == 80) {
		freq = (freq + (freq >> 2)) * util / max;
	} else if (target_load > 0 && target_load <= 100) {
		u64 tmp = (u64)freq * (u64)util * 100ULL;
		do_div(tmp, (u64)max * (u64)target_load);
		freq = (unsigned int)tmp;
	} else {
		freq = (freq + (freq >> 2)) * util / max;
	}

	trace_sugov_next_freq(policy->cpu, util, max, freq);

	if (freq == sg_policy->cached_raw_freq && sg_policy->next_freq != UINT_MAX)
		return sg_policy->next_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void sugov_ext_get_util(unsigned long *util, unsigned long *max, int cpu,
			       u64 time)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long max_cap, rt;
	struct sugov_ext_cpu *loadcpu = &per_cpu(sugov_ext_cpu, cpu);
	s64 delta;

	max_cap = arch_scale_cpu_capacity(cpu);
	*max = max_cap;

	*util = boosted_cpu_util(cpu, &loadcpu->walt_load);

	if (likely(sugov_ext_use_pelt())) {
		sched_avg_update(rq);
		delta = time - rq->age_stamp;
		if (unlikely(delta < 0))
			delta = 0;
		rt = div64_u64(rq->rt_avg, sched_avg_period() + delta);
		rt = (rt * max_cap) >> SCHED_CAPACITY_SHIFT;
		*util = min(*util + rt, max_cap);
	}

#ifdef CONFIG_UCLAMP_TASK
	*util = uclamp_rq_util_with(rq, *util, NULL);
#endif
}

static void sugov_ext_set_iowait_boost(struct sugov_ext_cpu *sg_cpu, u64 time,
				       unsigned int flags)
{
	if (!sg_cpu->sg_policy->tunables->iowait_boost_enable) {
		sg_cpu->iowait_boost = 0;
		sg_cpu->iowait_boost_pending = false;
		return;
	}

	if (flags & SCHED_CPUFREQ_IOWAIT) {
		if (sg_cpu->iowait_boost_pending)
			return;

		sg_cpu->iowait_boost_pending = true;

		if (sg_cpu->iowait_boost) {
			sg_cpu->iowait_boost <<= 1;
			if (sg_cpu->iowait_boost > sg_cpu->iowait_boost_max)
				sg_cpu->iowait_boost = sg_cpu->iowait_boost_max;
		} else {
			sg_cpu->iowait_boost = sg_cpu->sg_policy->policy->min;
		}
	} else if (sg_cpu->iowait_boost) {
		s64 delta_ns = time - sg_cpu->last_update;

		/* Clear iowait_boost if the CPU appears to have been idle. */
		if (delta_ns > TICK_NSEC) {
			sg_cpu->iowait_boost = 0;
			sg_cpu->iowait_boost_pending = false;
		}
	}
}

static void sugov_ext_iowait_boost(struct sugov_ext_cpu *sg_cpu, unsigned long *util,
				   unsigned long *max)
{
	unsigned int boost_util, boost_max;

	if (!sg_cpu->sg_policy->tunables->iowait_boost_enable || !sg_cpu->iowait_boost)
		return;

	if (sg_cpu->iowait_boost_pending) {
		sg_cpu->iowait_boost_pending = false;
	} else {
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < sg_cpu->sg_policy->policy->min) {
			sg_cpu->iowait_boost = 0;
			return;
		}
	}

	boost_util = sg_cpu->iowait_boost;
	boost_max = sg_cpu->iowait_boost_max;

	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_ext_cpu_is_busy(struct sugov_ext_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_ext_cpu_is_busy(struct sugov_ext_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

#define NL_RATIO 75
#define DEFAULT_HISPEED_LOAD 90
static void sugov_ext_walt_adjust(struct sugov_ext_cpu *sg_cpu, unsigned long *util,
				  unsigned long *max)
{
	struct sugov_ext_policy *sg_policy = sg_cpu->sg_policy;
	bool is_migration = sg_cpu->flags & SCHED_CPUFREQ_INTERCLUSTER_MIG;
	unsigned long nl = sg_cpu->walt_load.nl;
	unsigned long cpu_util = sg_cpu->util;
	bool is_hiload;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	is_hiload = (cpu_util >= mult_frac(sg_policy->avg_cap,
					   sg_policy->tunables->hispeed_load,
					   100));

	if (is_hiload && !is_migration)
		*util = max(*util, sg_policy->hispeed_util);

	if (is_hiload && nl >= mult_frac(cpu_util, NL_RATIO, 100))
		*util = max(*util, sg_policy->hispeed_util);

	if (sg_policy->tunables->pl)
		*util = max(*util, sg_cpu->walt_load.pl);
}

static void sugov_ext_update_single(struct update_util_data *hook, u64 time,
				    unsigned int flags)
{
	struct sugov_ext_cpu *sg_cpu = container_of(hook, struct sugov_ext_cpu, update_util);
	struct sugov_ext_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max, hs_util;
	unsigned int next_f;
	bool busy;

	if (!sg_policy->tunables->pl && (flags & SCHED_CPUFREQ_PL))
		return;

	flags &= ~SCHED_CPUFREQ_RT_DL;
	sugov_ext_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	if (!sugov_ext_should_update_freq(sg_policy, time))
		return;

	busy = sugov_ext_use_pelt() && sugov_ext_cpu_is_busy(sg_cpu);

	raw_spin_lock(&sg_policy->update_lock);

	if (flags & SCHED_CPUFREQ_DL) {
		sg_policy->cached_raw_freq = 0;
		next_f = policy->cpuinfo.max_freq;
	} else {
		sugov_ext_get_util(&util, &max, sg_cpu->cpu, time);
		if (sg_policy->max != max) {
			sg_policy->max = max;
			hs_util = sugov_ext_freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
			hs_util = mult_frac(hs_util, sg_policy->tunables->freq_margin_load, 100);
			sg_policy->hispeed_util = hs_util;
		}

		sg_cpu->util = util;
		sg_cpu->max = max;
		sg_cpu->flags = flags;

		sugov_ext_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
				       sg_policy->policy->cur);
		trace_sugov_util_update(sg_cpu->cpu, sg_cpu->util,
				sg_policy->avg_cap, max, sg_cpu->walt_load.nl,
				sg_cpu->walt_load.pl, flags);

		sugov_ext_iowait_boost(sg_cpu, &util, &max);
		sugov_ext_walt_adjust(sg_cpu, &util, &max);
		next_f = sugov_ext_get_next_freq(sg_policy, util, max);

		if (busy && next_f < sg_policy->next_freq &&
		    sg_policy->next_freq != UINT_MAX) {
			next_f = sg_policy->next_freq;
			sg_policy->cached_raw_freq = 0;
		}
	}
	sugov_ext_update_commit(sg_policy, time, next_f);
	raw_spin_unlock(&sg_policy->update_lock);
}

static unsigned int sugov_ext_next_freq_shared(struct sugov_ext_cpu *sg_cpu, u64 time)
{
	struct sugov_ext_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct sugov_ext_cpu *j_sg_cpu = &per_cpu(sugov_ext_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		delta_ns = time - j_sg_cpu->last_update;
		if (delta_ns > sugov_ext_stale_ns) {
			j_sg_cpu->iowait_boost = 0;
			j_sg_cpu->iowait_boost_pending = false;
			continue;
		}
		if (j_sg_cpu->flags & SCHED_CPUFREQ_DL) {
			sg_policy->cached_raw_freq = 0;
			return policy->cpuinfo.max_freq;
		}

		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (j_util * max >= j_max * util) {
			util = j_util;
			max = j_max;
		}

		sugov_ext_iowait_boost(j_sg_cpu, &util, &max);
		sugov_ext_walt_adjust(j_sg_cpu, &util, &max);
	}

	return sugov_ext_get_next_freq(sg_policy, util, max);
}

static void sugov_ext_update_shared(struct update_util_data *hook, u64 time,
				    unsigned int flags)
{
	struct sugov_ext_cpu *sg_cpu = container_of(hook, struct sugov_ext_cpu, update_util);
	struct sugov_ext_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max, hs_util;
	unsigned int next_f;

	if (!sg_policy->tunables->pl && (flags & SCHED_CPUFREQ_PL))
		return;

	sugov_ext_get_util(&util, &max, sg_cpu->cpu, time);

	flags &= ~SCHED_CPUFREQ_RT_DL;

	raw_spin_lock(&sg_policy->update_lock);

	if (sg_policy->max != max) {
		sg_policy->max = max;
		hs_util = sugov_ext_freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		hs_util = mult_frac(hs_util, sg_policy->tunables->freq_margin_load, 100);
		sg_policy->hispeed_util = hs_util;
	}

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;

	sugov_ext_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	sugov_ext_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
			       sg_policy->policy->cur);

	trace_sugov_util_update(sg_cpu->cpu, sg_cpu->util, sg_policy->avg_cap,
				max, sg_cpu->walt_load.nl,
				sg_cpu->walt_load.pl, flags);

	if (sugov_ext_should_update_freq(sg_policy, time) &&
	    !(flags & SCHED_CPUFREQ_CONTINUE)) {
		if (flags & SCHED_CPUFREQ_DL) {
			next_f = sg_policy->policy->cpuinfo.max_freq;
			sg_policy->cached_raw_freq = 0;
		} else {
			next_f = sugov_ext_next_freq_shared(sg_cpu, time);
		}

		sugov_ext_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_ext_work(struct kthread_work *work)
{
	struct sugov_ext_policy *sg_policy = container_of(work, struct sugov_ext_policy, work);
	unsigned long flags;

	mutex_lock(&sg_policy->work_lock);
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	sugov_ext_track_cycles(sg_policy, sg_policy->policy->cur,
			       sched_ktime_clock());
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);

	if (sugov_ext_use_pelt())
		sg_policy->work_in_progress = false;
}

static void sugov_ext_irq_work(struct irq_work *irq_work)
{
	struct sugov_ext_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_ext_policy, irq_work);
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sugov_ext_tunables *sugov_ext_global_tunables;
static DEFINE_MUTEX(sugov_ext_global_tunables_lock);

static inline struct sugov_ext_tunables *to_sugov_ext_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_ext_tunables, attr_set);
}

static DEFINE_MUTEX(sugov_ext_min_rate_lock);

static void sugov_ext_update_min_rate_limit_ns(struct sugov_ext_policy *sg_policy)
{
	mutex_lock(&sugov_ext_min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&sugov_ext_min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	struct sugov_ext_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		sugov_ext_update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	struct sugov_ext_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		sugov_ext_update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t hispeed_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_load);
}

static ssize_t hispeed_load_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtouint(buf, 10, &tunables->hispeed_load))
		return -EINVAL;

	tunables->hispeed_load = min(100U, tunables->hispeed_load);

	return count;
}

static ssize_t hispeed_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_freq);
}

static ssize_t hispeed_freq_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;
	struct sugov_ext_policy *sg_policy;
	unsigned long hs_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->hispeed_freq = val;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		hs_util = sugov_ext_freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		hs_util = mult_frac(hs_util, sg_policy->tunables->freq_margin_load, 100);
		sg_policy->hispeed_util = hs_util;
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t freq_margin_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->freq_margin_load);
}

static ssize_t freq_margin_load_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;
	struct sugov_ext_policy *sg_policy;
	unsigned long hs_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (val < 20 || val > 100)
		return -EINVAL;

	tunables->freq_margin_load = val;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		hs_util = sugov_ext_freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		hs_util = mult_frac(hs_util, val, 100);
		sg_policy->hispeed_util = hs_util;
		sg_policy->cached_raw_freq = 0;
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t boost_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->boost);
}

static ssize_t boost_store(struct gov_attr_set *attr_set, const char *buf,
			   size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->boost))
		return -EINVAL;

	return count;
}

static ssize_t pl_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->pl);
}

static ssize_t pl_store(struct gov_attr_set *attr_set, const char *buf,
			size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->pl))
		return -EINVAL;

	return count;
}

static ssize_t iowait_boost_enable_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->iowait_boost_enable);
}

static ssize_t iowait_boost_enable_store(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->iowait_boost_enable))
		return -EINVAL;

	return count;
}

static ssize_t fast_ramp_down_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->fast_ramp_down);
}

static ssize_t fast_ramp_down_store(struct gov_attr_set *attr_set, const char *buf,
				    size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->fast_ramp_down))
		return -EINVAL;

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr freq_margin_load = __ATTR_RW(freq_margin_load);
static struct governor_attr boost = __ATTR_RW(boost);
static struct governor_attr pl = __ATTR_RW(pl);
static struct governor_attr iowait_boost_enable = __ATTR_RW(iowait_boost_enable);
static struct governor_attr fast_ramp_down = __ATTR_RW(fast_ramp_down);

static struct attribute *sugov_ext_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&freq_margin_load.attr,
	&boost.attr,
	&pl.attr,
	&iowait_boost_enable.attr,
	&fast_ramp_down.attr,
	NULL
};

static void sugov_ext_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_sugov_ext_tunables(attr_set));
}

static struct kobj_type sugov_ext_tunables_ktype = {
	.default_attrs = sugov_ext_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_ext_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor sugov_ext_gov;

static struct sugov_ext_policy *sugov_ext_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_ext_policy_free(struct sugov_ext_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_ext_kthread_create(struct sugov_ext_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, sugov_ext_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov_ext:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov_ext thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;

	/* Kthread is bound to all CPUs by default */
	if (!policy->dvfs_possible_from_any_cpu)
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&sg_policy->irq_work, sugov_ext_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void sugov_ext_kthread_stop(struct sugov_ext_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_ext_tunables *sugov_ext_tunables_alloc(struct sugov_ext_policy *sg_policy)
{
	struct sugov_ext_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			sugov_ext_global_tunables = tunables;
	}
	return tunables;
}

static void sugov_ext_tunables_save(struct cpufreq_policy *policy,
				    struct sugov_ext_tunables *tunables)
{
	int cpu;
	struct sugov_ext_tunables *cached = per_cpu(sugov_ext_cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(sugov_ext_cached_tunables, cpu) = cached;
	}

	cached->pl = tunables->pl;
	cached->boost = tunables->boost;
	cached->hispeed_load = tunables->hispeed_load;
	cached->hispeed_freq = tunables->hispeed_freq;
	cached->freq_margin_load = tunables->freq_margin_load;
	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
	cached->iowait_boost_enable = tunables->iowait_boost_enable;
	cached->fast_ramp_down = tunables->fast_ramp_down;
}

static void sugov_ext_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		sugov_ext_global_tunables = NULL;
}

static void sugov_ext_tunables_restore(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy = policy->governor_data;
	struct sugov_ext_tunables *tunables = sg_policy->tunables;
	struct sugov_ext_tunables *cached = per_cpu(sugov_ext_cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->pl = cached->pl;
	tunables->boost = cached->boost;
	tunables->hispeed_load = cached->hispeed_load;
	tunables->hispeed_freq = cached->hispeed_freq;
	tunables->freq_margin_load = cached->freq_margin_load;
	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	tunables->iowait_boost_enable = cached->iowait_boost_enable;
	tunables->fast_ramp_down = cached->fast_ramp_down;
	sugov_ext_update_min_rate_limit_ns(sg_policy);
}

static int sugov_ext_init(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy;
	struct sugov_ext_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_ext_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_ext_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&sugov_ext_global_tunables_lock);

	if (sugov_ext_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = sugov_ext_global_tunables;

		gov_attr_set_get(&sugov_ext_global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_ext_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->up_rate_limit_us = 500;
	tunables->down_rate_limit_us = 4000;
	tunables->hispeed_load = DEFAULT_HISPEED_LOAD;
	tunables->hispeed_freq = 0;
	tunables->freq_margin_load = DEFAULT_TARGET_LOAD;
	tunables->boost = false;
	tunables->pl = false;
	tunables->iowait_boost_enable = false;
	tunables->fast_ramp_down = true;

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	sugov_ext_stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	sugov_ext_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_ext_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   policy->governor->name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&sugov_ext_global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_ext_clear_global_tunables();

stop_kthread:
	sugov_ext_kthread_stop(sg_policy);
	mutex_unlock(&sugov_ext_global_tunables_lock);

free_sg_policy:
	sugov_ext_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_ext_exit(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy = policy->governor_data;
	struct sugov_ext_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&sugov_ext_global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		sugov_ext_tunables_save(policy, tunables);
		sugov_ext_clear_global_tunables();
	}

	mutex_unlock(&sugov_ext_global_tunables_lock);

	sugov_ext_kthread_stop(sg_policy);
	sugov_ext_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_ext_start(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_ext_cpu *sg_cpu = &per_cpu(sugov_ext_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu = cpu;
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->flags = SCHED_CPUFREQ_DL;
		sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq / 2;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_ext_cpu *sg_cpu = &per_cpu(sugov_ext_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_ext_update_shared :
							sugov_ext_update_single);
	}
	return 0;
}

static void sugov_ext_stop(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void sugov_ext_limits(struct cpufreq_policy *policy)
{
	struct sugov_ext_policy *sg_policy = policy->governor_data;
	unsigned long flags;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		sugov_ext_track_cycles(sg_policy, sg_policy->policy->cur,
				       sched_ktime_clock());
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		sugov_ext_track_cycles(sg_policy, sg_policy->policy->cur,
				       ktime_get_ns());
		cpufreq_policy_apply_limits_fast(policy);
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	sg_policy->need_freq_update = true;
}

static struct cpufreq_governor sugov_ext_gov = {
	.name = "sugov_ext",
	.owner = THIS_MODULE,
	.dynamic_switching = true,
	.init = sugov_ext_init,
	.exit = sugov_ext_exit,
	.start = sugov_ext_start,
	.stop = sugov_ext_stop,
	.limits = sugov_ext_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SUGOV_EXT
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &sugov_ext_gov;
}
#endif

static int __init sugov_ext_register(void)
{
	return cpufreq_register_governor(&sugov_ext_gov);
}
fs_initcall(sugov_ext_register);
