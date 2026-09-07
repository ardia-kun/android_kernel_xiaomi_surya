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
	bool			step_down_freq;
	bool			smart_app_aware;
	unsigned int		vc_max_freq;
	unsigned int		game_target_load;

	/* AI Workload Detector & Governor v3 tunables */
	unsigned int		hysteresis_ms;		/* base hysteresis window (ms) */
	unsigned int		vc_target_load;		/* target load for video calls */
	unsigned int		social_max_freq;	/* max freq cap for social media */
	unsigned int		social_target_load;	/* target load for social media */
	unsigned int		stream_max_freq;	/* max freq cap for streaming */
	unsigned int		stream_target_load;	/* target load for streaming */
	bool			ai_forecasting;		/* predictive load momentum enable */
	unsigned int		ai_game_momentum;	/* gaming load momentum boost % (default 50) */
	unsigned int		ai_social_momentum;	/* social load momentum boost % (default 30) */
	unsigned int		ai_default_momentum;	/* default load momentum boost % (default 20) */
	unsigned int		ai_mode;		/* 0: Off, 1: Adaptive, 2: Perf, 3: Battery */
	unsigned int		ai_thermal_bias;	/* thermal protection bias (0: Off, 1: Auto) */
};

enum sugov_ext_workload {
	WORKLOAD_DEFAULT = 0,
	WORKLOAD_VIDEOCALL,
	WORKLOAD_GAMING,
	WORKLOAD_SOCIAL_MEDIA,
	WORKLOAD_STREAMING,
	NR_WORKLOAD_TYPES
};

#define SUGOV_EXT_MAX_DYNAMIC_PATTERNS 16
#define SUGOV_EXT_PATTERN_LEN 16

struct sugov_ext_dyn_pattern {
	char pattern[SUGOV_EXT_PATTERN_LEN];
};

struct sugov_ext_dyn_patterns {
	struct sugov_ext_dyn_pattern entries[SUGOV_EXT_MAX_DYNAMIC_PATTERNS];
	unsigned int count;
};

static struct sugov_ext_dyn_patterns dyn_workload_patterns[NR_WORKLOAD_TYPES];
static DEFINE_RWLOCK(dyn_patterns_lock);

static bool sugov_ext_match_dynamic(const char *comm, const char *leader,
				    enum sugov_ext_workload wl);

static bool sugov_ext_match_comm(const char *comm, const char *pattern)
{
	int i, j, plen;

	if (!comm || !pattern)
		return false;

	plen = 0;
	while (plen < 16 && pattern[plen])
		plen++;

	for (i = 0; i < 16 && comm[i]; i++) {
		if (i + plen > 16)
			break;
		for (j = 0; j < plen; j++) {
			char c = comm[i + j];
			char p = pattern[j];

			/* fast tolower for ASCII letters */
			if (c >= 'A' && c <= 'Z') c += 32;
			if (p >= 'A' && p <= 'Z') p += 32;
			if (c != p)
				break;
		}
		if (j == plen)
			return true;
	}
	return false;
}

/* Check pattern against both task->comm and task->group_leader->comm */
static bool sugov_ext_match_any(const char *comm, const char *leader, const char *pattern)
{
	return sugov_ext_match_comm(comm, pattern) ||
	       sugov_ext_match_comm(leader, pattern);
}

static bool sugov_ext_match_dynamic(const char *comm, const char *leader,
				    enum sugov_ext_workload wl)
{
	unsigned long flags;
	unsigned int i;
	bool matched = false;

	if (wl <= WORKLOAD_DEFAULT || wl >= NR_WORKLOAD_TYPES)
		return false;

	read_lock_irqsave(&dyn_patterns_lock, flags);
	for (i = 0; i < dyn_workload_patterns[wl].count; i++) {
		const char *p = dyn_workload_patterns[wl].entries[i].pattern;
		if (sugov_ext_match_any(comm, leader, p)) {
			matched = true;
			break;
		}
	}
	read_unlock_irqrestore(&dyn_patterns_lock, flags);

	return matched;
}

/*
 * Autonomous Behavioral Gaming Classifier:
 * Catches unlisted games, 3D engines, emulators, and custom game packages
 * by analyzing thread signatures, thread counts, and runtime demand.
 */
static bool sugov_ext_is_gaming_task(struct task_struct *task, const char *comm,
				     const char *leader_comm)
{
	bool is_render_thread = false;

	/* 1. Check known game strings on comm or leader */
	if (sugov_ext_match_dynamic(comm, leader_comm, WORKLOAD_GAMING) ||
	    sugov_ext_match_any(comm, leader_comm, "mobile.leg") ||
	    sugov_ext_match_any(comm, leader_comm, "freefire") ||
	    sugov_ext_match_any(comm, leader_comm, "pubg") ||
	    sugov_ext_match_any(comm, leader_comm, "genshin") ||
	    sugov_ext_match_any(comm, leader_comm, "mihoyo") ||
	    sugov_ext_match_any(comm, leader_comm, "roblox") ||
	    sugov_ext_match_any(comm, leader_comm, "minecra") ||
	    sugov_ext_match_any(comm, leader_comm, "codm") ||
	    sugov_ext_match_any(comm, leader_comm, "garena") ||
	    sugov_ext_match_any(comm, leader_comm, "asphalt") ||
	    sugov_ext_match_any(comm, leader_comm, "playdead") ||
	    sugov_ext_match_any(comm, leader_comm, "arenaofval") ||
	    sugov_ext_match_any(comm, leader_comm, "honkai") ||
	    sugov_ext_match_any(comm, leader_comm, "fortnite") ||
	    sugov_ext_match_any(comm, leader_comm, "brawlstar") ||
	    sugov_ext_match_any(comm, leader_comm, "clash") ||
	    sugov_ext_match_any(comm, leader_comm, "konami") ||
	    sugov_ext_match_any(comm, leader_comm, "efootball") ||
	    sugov_ext_match_any(comm, leader_comm, "fifa") ||
	    sugov_ext_match_any(comm, leader_comm, "ea.gp") ||
	    sugov_ext_match_any(comm, leader_comm, "netease") ||
	    sugov_ext_match_any(comm, leader_comm, "riot") ||
	    sugov_ext_match_any(comm, leader_comm, "wildrift") ||
	    sugov_ext_match_any(comm, leader_comm, "pokemon") ||
	    sugov_ext_match_any(comm, leader_comm, "aethersx2") ||
	    sugov_ext_match_any(comm, leader_comm, "ppsspp") ||
	    sugov_ext_match_any(comm, leader_comm, "dolphin") ||
	    sugov_ext_match_any(comm, leader_comm, "citra") ||
	    sugov_ext_match_any(comm, leader_comm, "skyline") ||
	    sugov_ext_match_any(comm, leader_comm, "yuzu") ||
	    sugov_ext_match_any(comm, leader_comm, "vita3k") ||
	    sugov_ext_match_any(comm, leader_comm, "winlator") ||
	    sugov_ext_match_any(comm, leader_comm, "box64") ||
	    sugov_ext_match_any(comm, leader_comm, "exagear"))
		return true;

	/* 2. Specific game engine / worker thread signatures */
	if (sugov_ext_match_comm(comm, "UnityMain") ||
	    sugov_ext_match_comm(comm, "UnityWorker") ||
	    sugov_ext_match_comm(comm, "JobWorker") ||
	    sugov_ext_match_comm(comm, "UnrealEng") ||
	    sugov_ext_match_comm(comm, "GameThread") ||
	    sugov_ext_match_comm(comm, "EmuThread") ||
	    sugov_ext_match_comm(comm, "VkQueue") ||
	    sugov_ext_match_comm(comm, "CRender"))
		return true;

	/* 3. Common render thread names with behavioral heuristic */
	if (sugov_ext_match_comm(comm, "RenderThread") ||
	    sugov_ext_match_comm(comm, "GLThread") ||
	    sugov_ext_match_comm(comm, "Mali Shader") ||
	    sugov_ext_match_comm(comm, "RHIThread"))
		is_render_thread = true;

	if (is_render_thread) {
		/*
		 * Behavioral heuristic:
		 * If a render thread is active, verify if it belongs to a gaming process:
		 * - Either the process has high thread count (>= 14 threads, typical for 3D engines)
		 *   AND sustained active utilization (task_util >= 120 / 1024), OR
		 * - The thread itself is demanding heavy continuous capacity (task_util >= 280).
		 * This eliminates false positives for basic 2D Android apps like Settings or Calculator.
		 */
		if (task->signal && task->signal->nr_threads >= 14 && task_util(task) >= 120)
			return true;
		if (task_util(task) >= 280)
			return true;
	}

	return false;
}

static bool sugov_ext_is_videocall_task(struct task_struct *task, const char *comm,
					const char *leader_comm)
{
	if (sugov_ext_match_dynamic(comm, leader_comm, WORKLOAD_VIDEOCALL) ||
	    sugov_ext_match_any(comm, leader_comm, "whatsa") ||
	    sugov_ext_match_any(comm, leader_comm, "telegra") ||
	    sugov_ext_match_any(comm, leader_comm, "zoom") ||
	    sugov_ext_match_any(comm, leader_comm, "skype") ||
	    sugov_ext_match_any(comm, leader_comm, "discord") ||
	    sugov_ext_match_any(comm, leader_comm, "orca") ||
	    sugov_ext_match_any(comm, leader_comm, "wechat") ||
	    sugov_ext_match_any(comm, leader_comm, "meet") ||
	    sugov_ext_match_any(comm, leader_comm, "viber") ||
	    sugov_ext_match_any(comm, leader_comm, "signal") ||
	    sugov_ext_match_any(comm, leader_comm, "teams") ||
	    sugov_ext_match_any(comm, leader_comm, "line") ||
	    sugov_ext_match_comm(comm, "voip") ||
	    sugov_ext_match_comm(comm, "VoIP") ||
	    sugov_ext_match_comm(comm, "webrtc") ||
	    sugov_ext_match_comm(comm, "WebRTC") ||
	    sugov_ext_match_comm(comm, "oipconnection") ||
	    sugov_ext_match_comm(comm, "oipmanager") ||
	    sugov_ext_match_comm(comm, "oipservice") ||
	    sugov_ext_match_comm(comm, "AecAudio") ||
	    sugov_ext_match_comm(comm, "WebRtcAudio"))
		return true;

	return false;
}

static bool sugov_ext_is_social_task(struct task_struct *task, const char *comm,
				     const char *leader_comm)
{
	if (sugov_ext_match_dynamic(comm, leader_comm, WORKLOAD_SOCIAL_MEDIA) ||
	    sugov_ext_match_any(comm, leader_comm, "instagram") ||
	    sugov_ext_match_any(comm, leader_comm, "tiktok") ||
	    sugov_ext_match_any(comm, leader_comm, "ugc.trill") ||
	    sugov_ext_match_any(comm, leader_comm, "facebook") ||
	    sugov_ext_match_any(comm, leader_comm, "twitter") ||
	    sugov_ext_match_any(comm, leader_comm, "x.android") ||
	    sugov_ext_match_any(comm, leader_comm, "snapchat") ||
	    sugov_ext_match_any(comm, leader_comm, "reddit") ||
	    sugov_ext_match_any(comm, leader_comm, "threads") ||
	    sugov_ext_match_any(comm, leader_comm, "pinterest") ||
	    sugov_ext_match_any(comm, leader_comm, "kwai"))
		return true;

	return false;
}

static bool sugov_ext_is_streaming_task(struct task_struct *task, const char *comm,
					const char *leader_comm)
{
	if (sugov_ext_match_dynamic(comm, leader_comm, WORKLOAD_STREAMING) ||
	    sugov_ext_match_any(comm, leader_comm, "youtube") ||
	    sugov_ext_match_any(comm, leader_comm, "netflix") ||
	    sugov_ext_match_any(comm, leader_comm, "spotify") ||
	    sugov_ext_match_any(comm, leader_comm, "vanced") ||
	    sugov_ext_match_any(comm, leader_comm, "revanced") ||
	    sugov_ext_match_any(comm, leader_comm, "bilibili") ||
	    sugov_ext_match_any(comm, leader_comm, "music") ||
	    sugov_ext_match_any(comm, leader_comm, "disney") ||
	    sugov_ext_match_any(comm, leader_comm, "primevid") ||
	    sugov_ext_match_any(comm, leader_comm, "vidio") ||
	    sugov_ext_match_any(comm, leader_comm, "twitch") ||
	    sugov_ext_match_any(comm, leader_comm, "appletv") ||
	    sugov_ext_match_any(comm, leader_comm, "crunchy") ||
	    sugov_ext_match_comm(comm, "NuPlayer") ||
	    sugov_ext_match_comm(comm, "ExoPlayer") ||
	    sugov_ext_match_comm(comm, "MediaCodec") ||
	    sugov_ext_match_comm(comm, "AudioTrack") ||
	    sugov_ext_match_comm(comm, "v4l2_videoc"))
		return true;

	return false;
}

static enum sugov_ext_workload sugov_ext_check_task(struct task_struct *task)
{
	const char *comm;
	const char *leader_comm = NULL;

	if (!task)
		return WORKLOAD_DEFAULT;

	comm = task->comm;
	rcu_read_lock();
	if (task->group_leader)
		leader_comm = task->group_leader->comm;
	rcu_read_unlock();

	if (sugov_ext_is_gaming_task(task, comm, leader_comm))
		return WORKLOAD_GAMING;

	if (sugov_ext_is_videocall_task(task, comm, leader_comm))
		return WORKLOAD_VIDEOCALL;

	if (sugov_ext_is_streaming_task(task, comm, leader_comm))
		return WORKLOAD_STREAMING;

	if (sugov_ext_is_social_task(task, comm, leader_comm))
		return WORKLOAD_SOCIAL_MEDIA;

	return WORKLOAD_DEFAULT;
}

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

	/* AI Workload Tracker & Load Forecasting */
	enum sugov_ext_workload active_workload;
	u64 workload_expiry_time;
	unsigned int workload_transitions[NR_WORKLOAD_TYPES];
	u64 last_transition_time;
	unsigned long prev_util;
	unsigned int momentum_hits;
	unsigned int decay_damp_hits;

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
	s64 up_delay = sg_policy->up_rate_delay_ns;
	s64 down_delay = sg_policy->down_rate_delay_ns;

	delta_ns = time - sg_policy->last_freq_update_time;

	/*
	 * Adaptive rate limiting based on active workload:
	 * - Gaming:    halve up_delay → faster frequency ramp for responsiveness
	 * - VideoCall: 1.5× down_delay → smoother power-saving transitions
	 * - Streaming: 2× down_delay → aggressive power saving during playback
	 * - Social:    1.25× down_delay → moderate power saving while scrolling
	 */
	switch (sg_policy->active_workload) {
	case WORKLOAD_GAMING:
		up_delay >>= 1;
		break;
	case WORKLOAD_VIDEOCALL:
		down_delay += (down_delay >> 1); /* 1.5× */
		break;
	case WORKLOAD_STREAMING:
		down_delay <<= 1; /* 2× */
		break;
	case WORKLOAD_SOCIAL_MEDIA:
		up_delay <<= 1;   /* 2× up_delay: resist frequency ramping */
		down_delay >>= 1; /* 0.5× down_delay: rapidly drop to lowest frequency */
		break;
	default:
		break;
	}

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < up_delay)
		return true;

	if (next_freq < sg_policy->next_freq) {
		if (sg_policy->tunables->fast_ramp_down &&
		    next_freq <= sg_policy->policy->min)
			return false;

		if (delta_ns < down_delay)
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

static u64 sugov_ext_hysteresis_ns(struct sugov_ext_policy *sg_policy,
				   enum sugov_ext_workload wl)
{
	unsigned int base_ms = sg_policy->tunables->hysteresis_ms;

	/*
	 * Per-workload hysteresis scaling:
	 * - Gaming:      2.5× base → game threads switch often, keep state longer
	 * - VideoCall:   1.5× base → VoIP threads are intermittent
	 * - SocialMedia: 1.0× base → scrolling pauses are short
	 * - Streaming:   1.0× base → playback is continuous
	 */
	switch (wl) {
	case WORKLOAD_GAMING:
		base_ms = base_ms * 5 / 2;
		break;
	case WORKLOAD_VIDEOCALL:
		base_ms = base_ms * 3 / 2;
		break;
	default:
		break;
	}

	return (u64)base_ms * NSEC_PER_MSEC;
}

static enum sugov_ext_workload sugov_ext_get_current_workload(struct sugov_ext_policy *sg_policy)
{
	u64 now = sched_ktime_clock();
	enum sugov_ext_workload detected;

	if (!sg_policy->tunables->smart_app_aware)
		return WORKLOAD_DEFAULT;

	/* AI Mode overrides */
	if (sg_policy->tunables->ai_mode == 0)
		return WORKLOAD_DEFAULT;
	if (sg_policy->tunables->ai_mode == 2)
		return WORKLOAD_GAMING;
	if (sg_policy->tunables->ai_mode == 3)
		return WORKLOAD_STREAMING;

	detected = sugov_ext_check_task(current);
	if (detected != WORKLOAD_DEFAULT) {
		/* Track state transitions */
		if (detected != sg_policy->active_workload) {
			sg_policy->workload_transitions[detected]++;
			sg_policy->last_transition_time = now;
		}
		sg_policy->active_workload = detected;
		sg_policy->workload_expiry_time = now +
			sugov_ext_hysteresis_ns(sg_policy, detected);
		return detected;
	}

	/* Hold previous state during hysteresis window */
	if (now < sg_policy->workload_expiry_time)
		return sg_policy->active_workload;

	/* Expired — transition back to default */
	if (sg_policy->active_workload != WORKLOAD_DEFAULT) {
		sg_policy->workload_transitions[WORKLOAD_DEFAULT]++;
		sg_policy->last_transition_time = now;
	}
	sg_policy->active_workload = WORKLOAD_DEFAULT;
	return WORKLOAD_DEFAULT;
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
	unsigned int base_freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;
	unsigned int target_load = sg_policy->tunables->freq_margin_load;
	unsigned int freq;
	u64 tmp;
	enum sugov_ext_workload wl = sugov_ext_get_current_workload(sg_policy);
	unsigned long eff_util = util;
	long util_delta;

	if (unlikely(sg_policy->tunables->boost))
		return policy->cpuinfo.max_freq;

	/*
	 * === AI PREDICTIVE LOAD FORECASTING (Momentum & Anti-Jitter Smoothing) ===
	 * Evaluates the first derivative of CPU utilization (dUtil/dt) to forecast
	 * demand ahead of frame deadline misses (zero-lag jump) and smooth frame decay.
	 */
	if (sg_policy->tunables->ai_forecasting && sg_policy->tunables->ai_mode != 0) {
		util_delta = (long)util - (long)sg_policy->prev_util;

		if (util_delta > 0) {
			unsigned int momentum_pct = 0;

			switch (wl) {
			case WORKLOAD_GAMING:
				momentum_pct = sg_policy->tunables->ai_game_momentum;
				break;
			case WORKLOAD_SOCIAL_MEDIA:
				momentum_pct = 0; /* Strictly NO boost for social media: run at lowest freq */
				break;
			case WORKLOAD_DEFAULT:
				momentum_pct = sg_policy->tunables->ai_default_momentum;
				break;
			case WORKLOAD_VIDEOCALL:
			case WORKLOAD_STREAMING:
			default:
				momentum_pct = 0; /* Keep steady for battery efficiency */
				break;
			}

			if (momentum_pct > 0) {
				unsigned long boost = mult_frac(util_delta, momentum_pct, 100);
				eff_util = min(max, util + boost);
				sg_policy->momentum_hits++;
			}
		} else if (util_delta < 0 && wl == WORKLOAD_GAMING) {
			/*
			 * Anti-Jitter Frame Smoothing for Gaming:
			 * Limit sudden utilization drop between frame renders (e.g. max 25% drop)
			 * to prevent micro-stutters when the subsequent frame arrives.
			 */
			unsigned long floor = sg_policy->prev_util > (sg_policy->prev_util >> 2) ?
					      sg_policy->prev_util - (sg_policy->prev_util >> 2) : 0;
			if (util < floor) {
				eff_util = floor;
				sg_policy->decay_damp_hits++;
			}
		}
	}
	sg_policy->prev_util = util;

	/*
	 * Intelligent Workload Tuning:
	 * - Gaming:      Lower target load → faster ramp-up for responsiveness
	 * - Video Call:  Higher target load + freq cap → cool operation & battery
	 * - Social:      Max efficiency target load → restrict strictly to lowest freq
	 * - Streaming:   Maximum efficiency → sustained playback with lowest power
	 */
	switch (wl) {
	case WORKLOAD_GAMING:
		target_load = sg_policy->tunables->game_target_load;
		break;
	case WORKLOAD_VIDEOCALL:
		target_load = max(target_load, sg_policy->tunables->vc_target_load);
		break;
	case WORKLOAD_SOCIAL_MEDIA:
		target_load = max(target_load, sg_policy->tunables->social_target_load);
		break;
	case WORKLOAD_STREAMING:
		target_load = max(target_load, sg_policy->tunables->stream_target_load);
		break;
	default:
		break;
	}

	/* Thermal adaptive scaling bias: soften target load when thermal headroom is constrained */
	if (sg_policy->tunables->ai_thermal_bias && arch_scale_cpu_capacity(policy->cpu) < 950)
		target_load = min(100U, target_load + 8);

	if (unlikely(target_load < 20 || target_load > 100))
		target_load = DEFAULT_TARGET_LOAD;

	/* 64-bit precise calculation: freq = base_freq * (eff_util / max) * (100 / target_load) */
	tmp = (u64)base_freq * (u64)eff_util * 100ULL;
	do_div(tmp, (u64)max * (u64)target_load);
	freq = (unsigned int)tmp;

	/* Ensure hispeed_freq jump if util reaches hispeed_load threshold (bypassed for social media) */
	if (wl != WORKLOAD_SOCIAL_MEDIA && sg_policy->tunables->hispeed_freq &&
	    eff_util >= mult_frac(max, sg_policy->tunables->hispeed_load, 100)) {
		freq = max(freq, sg_policy->tunables->hispeed_freq);
	}

	/* Workload-specific frequency capping: save battery & reduce thermals */
	if (wl == WORKLOAD_VIDEOCALL && sg_policy->tunables->vc_max_freq) {
		freq = min(freq, sg_policy->tunables->vc_max_freq);
	} else if (wl == WORKLOAD_SOCIAL_MEDIA && sg_policy->tunables->social_max_freq) {
		freq = min(freq, sg_policy->tunables->social_max_freq);
	} else if (wl == WORKLOAD_STREAMING && sg_policy->tunables->stream_max_freq) {
		freq = min(freq, sg_policy->tunables->stream_max_freq);
	}

	/* Optional step-down gradual scaling under non-idle load */
	if (sg_policy->tunables->step_down_freq &&
	    sg_policy->next_freq != UINT_MAX && freq < sg_policy->next_freq) {
		unsigned int step = (sg_policy->next_freq - freq) >> 1;
		if (step > 0 && (sg_policy->next_freq - step) > policy->min)
			freq = sg_policy->next_freq - step;
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
			sg_cpu->iowait_boost += (sg_cpu->iowait_boost_max >> 2);
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

static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t rate_limit_us_store(struct gov_attr_set *attr_set,
				   const char *buf, size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	struct sugov_ext_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;
	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
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

	tunables->hispeed_load = clamp_val(tunables->hispeed_load, 1U, 100U);

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

static ssize_t step_down_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->step_down_freq);
}

static ssize_t step_down_freq_store(struct gov_attr_set *attr_set, const char *buf,
				    size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->step_down_freq))
		return -EINVAL;

	return count;
}

static ssize_t smart_app_aware_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->smart_app_aware);
}

static ssize_t smart_app_aware_store(struct gov_attr_set *attr_set, const char *buf,
				     size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	if (kstrtobool(buf, &tunables->smart_app_aware))
		return -EINVAL;

	return count;
}

static ssize_t vc_max_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->vc_max_freq);
}

static ssize_t vc_max_freq_store(struct gov_attr_set *attr_set, const char *buf,
				 size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->vc_max_freq = val;
	return count;
}

static ssize_t game_target_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->game_target_load);
}

static ssize_t game_target_load_store(struct gov_attr_set *attr_set, const char *buf,
				      size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->game_target_load = clamp_val(val, 20U, 100U);
	return count;
}

static ssize_t current_workload_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_policy *sg_policy;
	static const char * const wl_names[] = {
		[WORKLOAD_DEFAULT]      = "Default",
		[WORKLOAD_VIDEOCALL]    = "VideoCall",
		[WORKLOAD_GAMING]       = "Gaming",
		[WORKLOAD_SOCIAL_MEDIA] = "SocialMedia",
		[WORKLOAD_STREAMING]    = "Streaming",
	};
	const char *str = "Default";

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		enum sugov_ext_workload wl = sg_policy->active_workload;

		if (wl > WORKLOAD_DEFAULT && wl < NR_WORKLOAD_TYPES) {
			str = wl_names[wl];
			break;
		}
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", str);
}

static ssize_t workload_stats_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_policy *sg_policy;
	int len = 0;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"policy%u:\n"
			"  Default->%u VideoCall->%u Gaming->%u Social->%u Stream->%u\n"
			"  active: %u  last_change: %llu ns ago\n"
			"  momentum_hits: %u  decay_damp_hits: %u\n",
			sg_policy->policy->cpu,
			sg_policy->workload_transitions[WORKLOAD_DEFAULT],
			sg_policy->workload_transitions[WORKLOAD_VIDEOCALL],
			sg_policy->workload_transitions[WORKLOAD_GAMING],
			sg_policy->workload_transitions[WORKLOAD_SOCIAL_MEDIA],
			sg_policy->workload_transitions[WORKLOAD_STREAMING],
			sg_policy->active_workload,
			sg_policy->last_transition_time ?
				sched_ktime_clock() - sg_policy->last_transition_time : 0,
			sg_policy->momentum_hits,
			sg_policy->decay_damp_hits);
		break; /* first policy is enough for global tunables */
	}

	return len;
}

static ssize_t hysteresis_ms_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hysteresis_ms);
}

static ssize_t hysteresis_ms_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->hysteresis_ms = clamp_val(val, 500U, 15000U);
	return count;
}

static ssize_t vc_target_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->vc_target_load);
}

static ssize_t vc_target_load_store(struct gov_attr_set *attr_set, const char *buf,
				    size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->vc_target_load = clamp_val(val, 20U, 100U);
	return count;
}

static ssize_t social_max_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->social_max_freq);
}

static ssize_t social_max_freq_store(struct gov_attr_set *attr_set, const char *buf,
				     size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->social_max_freq = val;
	return count;
}

static ssize_t social_target_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->social_target_load);
}

static ssize_t social_target_load_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->social_target_load = clamp_val(val, 20U, 100U);
	return count;
}

static ssize_t stream_max_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->stream_max_freq);
}

static ssize_t stream_max_freq_store(struct gov_attr_set *attr_set, const char *buf,
				     size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->stream_max_freq = val;
	return count;
}

static ssize_t stream_target_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->stream_target_load);
}

static ssize_t stream_target_load_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->stream_target_load = clamp_val(val, 20U, 100U);
	return count;
}

static ssize_t ai_forecasting_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->ai_forecasting);
}

static ssize_t ai_forecasting_store(struct gov_attr_set *attr_set, const char *buf,
				    size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	bool enable;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	tunables->ai_forecasting = enable;
	return count;
}

static ssize_t ai_game_momentum_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->ai_game_momentum);
}

static ssize_t ai_game_momentum_store(struct gov_attr_set *attr_set, const char *buf,
				      size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->ai_game_momentum = clamp_val(val, 0U, 100U);
	return count;
}

static ssize_t ai_social_momentum_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->ai_social_momentum);
}

static ssize_t ai_social_momentum_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->ai_social_momentum = clamp_val(val, 0U, 100U);
	return count;
}

static ssize_t ai_default_momentum_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->ai_default_momentum);
}

static ssize_t ai_default_momentum_store(struct gov_attr_set *attr_set, const char *buf,
					 size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->ai_default_momentum = clamp_val(val, 0U, 100U);
	return count;
}

static ssize_t ai_mode_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	static const char * const modes[] = {
		"0 (Disabled)",
		"1 (Adaptive - Balanced)",
		"2 (Performance)",
		"3 (Battery)"
	};
	unsigned int m = tunables->ai_mode;

	if (m > 3)
		m = 1;

	return scnprintf(buf, PAGE_SIZE, "%s\n", modes[m]);
}

static ssize_t ai_mode_store(struct gov_attr_set *attr_set, const char *buf,
			     size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->ai_mode = clamp_val(val, 0U, 3U);
	return count;
}

static ssize_t ai_thermal_bias_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->ai_thermal_bias);
}

static ssize_t ai_thermal_bias_store(struct gov_attr_set *attr_set, const char *buf,
				     size_t count)
{
	struct sugov_ext_tunables *tunables = to_sugov_ext_tunables(attr_set);
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->ai_thermal_bias = clamp_val(val, 0U, 1U);
	return count;
}

static ssize_t sugov_ext_dyn_show(enum sugov_ext_workload wl, char *buf)
{
	unsigned long flags;
	unsigned int i;
	int len = 0;

	if (wl <= WORKLOAD_DEFAULT || wl >= NR_WORKLOAD_TYPES)
		return -EINVAL;

	read_lock_irqsave(&dyn_patterns_lock, flags);
	for (i = 0; i < dyn_workload_patterns[wl].count; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				 i > 0 ? " " : "",
				 dyn_workload_patterns[wl].entries[i].pattern);
	}
	read_unlock_irqrestore(&dyn_patterns_lock, flags);

	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t sugov_ext_dyn_store(enum sugov_ext_workload wl, const char *buf, size_t count)
{
	unsigned long flags;
	char *tmp, *orig, *token;

	if (wl <= WORKLOAD_DEFAULT || wl >= NR_WORKLOAD_TYPES)
		return -EINVAL;

	orig = kstrdup(buf, GFP_KERNEL);
	if (!orig)
		return -ENOMEM;

	tmp = strim(orig);

	write_lock_irqsave(&dyn_patterns_lock, flags);

	/* If writing "clear", "none", or empty string, wipe all patterns for this workload */
	if (!*tmp || !strcmp(tmp, "clear") || !strcmp(tmp, "none")) {
		dyn_workload_patterns[wl].count = 0;
		memset(dyn_workload_patterns[wl].entries, 0, sizeof(dyn_workload_patterns[wl].entries));
		write_unlock_irqrestore(&dyn_patterns_lock, flags);
		kfree(orig);
		return count;
	}

	/* Reset list and populate with new space/newline-delimited tokens */
	dyn_workload_patterns[wl].count = 0;
	memset(dyn_workload_patterns[wl].entries, 0, sizeof(dyn_workload_patterns[wl].entries));

	while ((token = strsep(&tmp, " \t\n,")) != NULL) {
		if (!*token)
			continue;

		if (dyn_workload_patterns[wl].count >= SUGOV_EXT_MAX_DYNAMIC_PATTERNS)
			break;

		strlcpy(dyn_workload_patterns[wl].entries[dyn_workload_patterns[wl].count].pattern,
			token, SUGOV_EXT_PATTERN_LEN);
		dyn_workload_patterns[wl].count++;
	}

	write_unlock_irqrestore(&dyn_patterns_lock, flags);
	kfree(orig);
	return count;
}

static ssize_t custom_gaming_apps_show(struct gov_attr_set *attr_set, char *buf)
{
	return sugov_ext_dyn_show(WORKLOAD_GAMING, buf);
}

static ssize_t custom_gaming_apps_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	return sugov_ext_dyn_store(WORKLOAD_GAMING, buf, count);
}

static ssize_t custom_videocall_apps_show(struct gov_attr_set *attr_set, char *buf)
{
	return sugov_ext_dyn_show(WORKLOAD_VIDEOCALL, buf);
}

static ssize_t custom_videocall_apps_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	return sugov_ext_dyn_store(WORKLOAD_VIDEOCALL, buf, count);
}

static ssize_t custom_social_apps_show(struct gov_attr_set *attr_set, char *buf)
{
	return sugov_ext_dyn_show(WORKLOAD_SOCIAL_MEDIA, buf);
}

static ssize_t custom_social_apps_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	return sugov_ext_dyn_store(WORKLOAD_SOCIAL_MEDIA, buf, count);
}

static ssize_t custom_streaming_apps_show(struct gov_attr_set *attr_set, char *buf)
{
	return sugov_ext_dyn_show(WORKLOAD_STREAMING, buf);
}

static ssize_t custom_streaming_apps_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	return sugov_ext_dyn_store(WORKLOAD_STREAMING, buf, count);
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr rate_limit_us = __ATTR_RW(rate_limit_us);
static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr freq_margin_load = __ATTR_RW(freq_margin_load);
static struct governor_attr boost = __ATTR_RW(boost);
static struct governor_attr pl = __ATTR_RW(pl);
static struct governor_attr iowait_boost_enable = __ATTR_RW(iowait_boost_enable);
static struct governor_attr fast_ramp_down = __ATTR_RW(fast_ramp_down);
static struct governor_attr step_down_freq = __ATTR_RW(step_down_freq);
static struct governor_attr smart_app_aware = __ATTR_RW(smart_app_aware);
static struct governor_attr vc_max_freq = __ATTR_RW(vc_max_freq);
static struct governor_attr game_target_load = __ATTR_RW(game_target_load);
static struct governor_attr current_workload = __ATTR_RO(current_workload);
static struct governor_attr workload_stats = __ATTR_RO(workload_stats);
static struct governor_attr hysteresis_ms = __ATTR_RW(hysteresis_ms);
static struct governor_attr vc_target_load = __ATTR_RW(vc_target_load);
static struct governor_attr social_max_freq = __ATTR_RW(social_max_freq);
static struct governor_attr social_target_load = __ATTR_RW(social_target_load);
static struct governor_attr stream_max_freq = __ATTR_RW(stream_max_freq);
static struct governor_attr stream_target_load = __ATTR_RW(stream_target_load);
static struct governor_attr custom_gaming_apps = __ATTR_RW(custom_gaming_apps);
static struct governor_attr custom_videocall_apps = __ATTR_RW(custom_videocall_apps);
static struct governor_attr custom_social_apps = __ATTR_RW(custom_social_apps);
static struct governor_attr custom_streaming_apps = __ATTR_RW(custom_streaming_apps);
static struct governor_attr ai_forecasting = __ATTR_RW(ai_forecasting);
static struct governor_attr ai_game_momentum = __ATTR_RW(ai_game_momentum);
static struct governor_attr ai_social_momentum = __ATTR_RW(ai_social_momentum);
static struct governor_attr ai_default_momentum = __ATTR_RW(ai_default_momentum);
static struct governor_attr ai_mode = __ATTR_RW(ai_mode);
static struct governor_attr ai_thermal_bias = __ATTR_RW(ai_thermal_bias);

static struct attribute *sugov_ext_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&rate_limit_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&freq_margin_load.attr,
	&boost.attr,
	&pl.attr,
	&iowait_boost_enable.attr,
	&fast_ramp_down.attr,
	&step_down_freq.attr,
	&smart_app_aware.attr,
	&vc_max_freq.attr,
	&game_target_load.attr,
	&current_workload.attr,
	&workload_stats.attr,
	&hysteresis_ms.attr,
	&vc_target_load.attr,
	&social_max_freq.attr,
	&social_target_load.attr,
	&stream_max_freq.attr,
	&stream_target_load.attr,
	&custom_gaming_apps.attr,
	&custom_videocall_apps.attr,
	&custom_social_apps.attr,
	&custom_streaming_apps.attr,
	&ai_forecasting.attr,
	&ai_game_momentum.attr,
	&ai_social_momentum.attr,
	&ai_default_momentum.attr,
	&ai_mode.attr,
	&ai_thermal_bias.attr,
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
	cached->step_down_freq = tunables->step_down_freq;
	cached->smart_app_aware = tunables->smart_app_aware;
	cached->vc_max_freq = tunables->vc_max_freq;
	cached->game_target_load = tunables->game_target_load;
	cached->hysteresis_ms = tunables->hysteresis_ms;
	cached->vc_target_load = tunables->vc_target_load;
	cached->social_max_freq = tunables->social_max_freq;
	cached->social_target_load = tunables->social_target_load;
	cached->stream_max_freq = tunables->stream_max_freq;
	cached->stream_target_load = tunables->stream_target_load;
	cached->ai_forecasting = tunables->ai_forecasting;
	cached->ai_game_momentum = tunables->ai_game_momentum;
	cached->ai_social_momentum = tunables->ai_social_momentum;
	cached->ai_default_momentum = tunables->ai_default_momentum;
	cached->ai_mode = tunables->ai_mode;
	cached->ai_thermal_bias = tunables->ai_thermal_bias;
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
	tunables->step_down_freq = cached->step_down_freq;
	tunables->smart_app_aware = cached->smart_app_aware;
	tunables->vc_max_freq = cached->vc_max_freq;
	tunables->game_target_load = cached->game_target_load;
	tunables->hysteresis_ms = cached->hysteresis_ms;
	tunables->vc_target_load = cached->vc_target_load;
	tunables->social_max_freq = cached->social_max_freq;
	tunables->social_target_load = cached->social_target_load;
	tunables->stream_max_freq = cached->stream_max_freq;
	tunables->stream_target_load = cached->stream_target_load;
	tunables->ai_forecasting = cached->ai_forecasting;
	tunables->ai_game_momentum = cached->ai_game_momentum;
	tunables->ai_social_momentum = cached->ai_social_momentum;
	tunables->ai_default_momentum = cached->ai_default_momentum;
	tunables->ai_mode = cached->ai_mode;
	tunables->ai_thermal_bias = cached->ai_thermal_bias;
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

	/* Intelligent default configuration tuned for SM7150 Kryo big.LITTLE */
	tunables->freq_margin_load = DEFAULT_TARGET_LOAD;
	tunables->boost = false;
	tunables->pl = false;
	tunables->iowait_boost_enable = false;
	tunables->fast_ramp_down = true;
	tunables->step_down_freq = false;
	tunables->smart_app_aware = true;
	tunables->game_target_load = 75;
	tunables->hysteresis_ms = 2000;
	tunables->vc_target_load = 90;
	tunables->social_target_load = 98;
	tunables->stream_target_load = 92;
	tunables->ai_forecasting = true;
	tunables->ai_game_momentum = 50;
	tunables->ai_social_momentum = 0; /* Strictly NO boost for social media */
	tunables->ai_default_momentum = 20;
	tunables->ai_mode = 1; /* Auto-Adaptive */
	tunables->ai_thermal_bias = 1;

	if (policy->cpuinfo.max_freq > 2000000) {
		/* Big Cluster (Gold cores, e.g. 2.3GHz) - Cooler & Battery friendly */
		tunables->up_rate_limit_us = 2000;
		tunables->down_rate_limit_us = 8000;
		tunables->hispeed_load = 92;
		tunables->hispeed_freq = cpufreq_driver_resolve_freq(policy, 1536000);
		tunables->vc_max_freq = cpufreq_driver_resolve_freq(policy, 1536000);
		tunables->social_max_freq = policy->min; /* Strictly cap Big cores at lowest frequency */
		tunables->stream_max_freq = cpufreq_driver_resolve_freq(policy, 1401600);
	} else {
		/* Little Cluster (Silver cores, e.g. 1.8GHz) - Smooth daily UI */
		tunables->up_rate_limit_us = 1000;
		tunables->down_rate_limit_us = 6000;
		tunables->hispeed_load = 88;
		tunables->hispeed_freq = cpufreq_driver_resolve_freq(policy, 1324800);
		tunables->vc_max_freq = cpufreq_driver_resolve_freq(policy, 1209600);
		tunables->social_max_freq = cpufreq_driver_resolve_freq(policy, 768000); /* Cap Little cores at lowest step (768MHz) */
		tunables->stream_max_freq = cpufreq_driver_resolve_freq(policy, 1209600);
	}

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
