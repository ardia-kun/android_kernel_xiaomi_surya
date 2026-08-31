// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Common Applications Kept Enhanced (CAKE) Qdisc
 *
 * Copyright (C) 2014-2018 Jonathan Morton <chromatix99@gmail.com>
 * Copyright (C) 2015-2018 Toke Høiland-Jørgensen <toke@toke.dk>
 * Copyright (C) 2016-2018 Dave Täht <dave.taht@gmail.com>
 * Copyright (C) 2015-2018 Kevin Darbyshire-Bryant <kevin@darbyshire-bryant.me.uk>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/reciprocal_div.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/tcp.h>
#include <net/flow_dissector.h>

#define CAKE_MAX_TINS (8)
#define CAKE_QUEUES (1024)
#define CAKE_FLOW_MASK 1023
#define CAKE_SET_WAYS (8)

struct cake_flow {
	struct sk_buff *head;
	struct sk_buff *tail;
	struct list_head flowchain;
	s32 deficit;
	u32 dropped;
	u16 set;
	u16 srchost;
	u16 dsthost;
	u8  headroom;
};

struct cake_tin {
	struct list_head new_flows;
	struct list_head old_flows;
	struct list_head decaying_flows;
	u64 time_next_packet;
	u32 backlogged_packets;
	u32 backlogged_bytes;
	u32 tin_rate_bps;
	u16 tin_weight;
	u16 bulk_flow_count;
	u16 unresp_flow_count;
	struct cake_flow flows[CAKE_QUEUES];
};

struct cake_sched_data {
	struct cake_tin *tins;
	u8 cur_tin;
	u8 tin_cnt;
	u8 tin_mode;
	u8 flow_mode;
	u8 ack_filter;
	u8 atm_mode;
	u32 buffer_limit;
	u32 buffer_used;
	u32 target;
	u32 interval;
	u32 rate_bps;
	u16 overhead;
	u16 mpu;
	struct qdisc_watchdog watchdog;
};

static void cake_reconfigure(struct Qdisc *sch);

static int cake_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int i, j;

	q->tin_cnt = 3;
	q->tin_mode = CAKE_DIFFSERV_DIFFSERV3;
	q->flow_mode = CAKE_FLOW_TRIPLE;
	q->target = 5000; /* 5ms */
	q->interval = 100000; /* 100ms */
	q->buffer_limit = 1024 * 1024 * 4; /* 4MB */
	q->rate_bps = 0; /* unlimited / wire speed */

	q->tins = kvzalloc(sizeof(struct cake_tin) * CAKE_MAX_TINS, GFP_KERNEL);
	if (!q->tins)
		return -ENOMEM;

	for (i = 0; i < CAKE_MAX_TINS; i++) {
		struct cake_tin *tin = &q->tins[i];
		INIT_LIST_HEAD(&tin->new_flows);
		INIT_LIST_HEAD(&tin->old_flows);
		INIT_LIST_HEAD(&tin->decaying_flows);
		tin->tin_weight = 1;
		for (j = 0; j < CAKE_QUEUES; j++) {
			INIT_LIST_HEAD(&tin->flows[j].flowchain);
			tin->flows[j].deficit = 1514;
		}
	}

	qdisc_watchdog_init(&q->watchdog, sch);
	return 0;
}

static void cake_destroy(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int i, j;

	qdisc_watchdog_cancel(&q->watchdog);

	if (q->tins) {
		for (i = 0; i < CAKE_MAX_TINS; i++) {
			struct cake_tin *tin = &q->tins[i];
			for (j = 0; j < CAKE_QUEUES; j++) {
				struct sk_buff *skb = tin->flows[j].head;
				while (skb) {
					struct sk_buff *next = skb->next;
					kfree_skb(skb);
					skb = next;
				}
			}
		}
		kvfree(q->tins);
		q->tins = NULL;
	}
}

static void cake_reset(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int i, j;

	qdisc_watchdog_cancel(&q->watchdog);

	if (q->tins) {
		for (i = 0; i < CAKE_MAX_TINS; i++) {
			struct cake_tin *tin = &q->tins[i];
			INIT_LIST_HEAD(&tin->new_flows);
			INIT_LIST_HEAD(&tin->old_flows);
			INIT_LIST_HEAD(&tin->decaying_flows);
			tin->backlogged_packets = 0;
			tin->backlogged_bytes = 0;
			for (j = 0; j < CAKE_QUEUES; j++) {
				struct sk_buff *skb = tin->flows[j].head;
				while (skb) {
					struct sk_buff *next = skb->next;
					kfree_skb(skb);
					skb = next;
				}
				tin->flows[j].head = NULL;
				tin->flows[j].tail = NULL;
				INIT_LIST_HEAD(&tin->flows[j].flowchain);
				tin->flows[j].deficit = 1514;
			}
		}
	}
	sch->q.qlen = 0;
	q->buffer_used = 0;
}

static u32 cake_hash(struct cake_sched_data *q, struct sk_buff *skb)
{
	u32 hash = skb_get_hash(skb);
	if (!hash)
		hash = (u32)(unsigned long)skb;
	return hash & CAKE_FLOW_MASK;
}

static u8 cake_classify_tin(struct cake_sched_data *q, struct sk_buff *skb)
{
	u8 dscp = 0;
	if (skb->protocol == htons(ETH_P_IP) && ip_hdr(skb))
		dscp = ip_hdr(skb)->tos >> 2;
	else if (skb->protocol == htons(ETH_P_IPV6) && ipv6_hdr(skb))
		dscp = ipv6_get_dsfield(ipv6_hdr(skb)) >> 2;

	if (q->tin_cnt == 3) {
		if (dscp >= 32)
			return 0; /* Voice / High priority */
		else if (dscp >= 8)
			return 1; /* Best effort */
		else
			return 2; /* Bulk */
	}
	return 0;
}

static int cake_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			struct sk_buff **to_free)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	u8 tin_idx = cake_classify_tin(q, skb);
	struct cake_tin *tin = &q->tins[tin_idx];
	u32 flow_idx = cake_hash(q, skb);
	struct cake_flow *flow = &tin->flows[flow_idx];
	u32 len = qdisc_pkt_len(skb);

	if (unlikely(q->buffer_used + len > q->buffer_limit)) {
		qdisc_qstats_overlimit(sch);
		return qdisc_drop(skb, sch, to_free);
	}

	skb->next = NULL;
	if (!flow->head) {
		flow->head = skb;
		flow->tail = skb;
		list_add_tail(&flow->flowchain, &tin->new_flows);
	} else {
		flow->tail->next = skb;
		flow->tail = skb;
	}

	tin->backlogged_packets++;
	tin->backlogged_bytes += len;
	q->buffer_used += len;
	sch->q.qlen++;
	qdisc_qstats_backlog_inc(sch, skb);

	return NET_XMIT_SUCCESS;
}

static struct sk_buff *cake_dequeue(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int t;

	for (t = 0; t < q->tin_cnt; t++) {
		struct cake_tin *tin = &q->tins[t];
		struct list_head *head = &tin->new_flows;
		struct cake_flow *flow;
		struct sk_buff *skb;

		if (list_empty(head))
			head = &tin->old_flows;
		if (list_empty(head))
			continue;

		flow = list_first_entry(head, struct cake_flow, flowchain);
		skb = flow->head;
		if (skb) {
			u32 len = qdisc_pkt_len(skb);
			flow->head = skb->next;
			if (!flow->head)
				flow->tail = NULL;
			skb->next = NULL;

			tin->backlogged_packets--;
			tin->backlogged_bytes -= len;
			q->buffer_used -= len;
			sch->q.qlen--;
			qdisc_qstats_backlog_dec(sch, skb);
			qdisc_bstats_update(sch, skb);

			if (!flow->head)
				list_del_init(&flow->flowchain);
			else if (head == &tin->new_flows)
				list_move_tail(&flow->flowchain, &tin->old_flows);

			return skb;
		}
	}
	return NULL;
}

static int cake_change(struct Qdisc *sch, struct nlattr *opt)
{
	if (!opt)
		return -EINVAL;

	/* Options parsing logic can be expanded here */
	return 0;
}

static int cake_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start(skb, TCA_OPTIONS);
	if (!opts)
		return -EMSGSIZE;

	if (nla_put_u32(skb, TCA_CAKE_TARGET, q->target) ||
	    nla_put_u32(skb, TCA_CAKE_RTT, q->interval) ||
	    nla_put_u32(skb, TCA_CAKE_MEMORY, q->buffer_limit) ||
	    nla_put_u32(skb, TCA_CAKE_DIFFSERV_MODE, q->tin_mode) ||
	    nla_put_u32(skb, TCA_CAKE_FLOW_MODE, q->flow_mode))
		return -EMSGSIZE;

	return nla_nest_end(skb, opts);
}

static int cake_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct tc_cake_xstats st = {
		.version = 1,
		.max_tins = CAKE_MAX_TINS,
		.tin_cnt = q->tin_cnt,
		.memory_limit = q->buffer_limit,
		.memory_used = q->buffer_used,
	};

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc_ops cake_qdisc_ops __read_mostly = {
	.id		= "cake",
	.priv_size	= sizeof(struct cake_sched_data),
	.init		= cake_init,
	.destroy	= cake_destroy,
	.reset		= cake_reset,
	.change		= cake_change,
	.enqueue	= cake_enqueue,
	.dequeue	= cake_dequeue,
	.peek		= qdisc_peek_dequeued,
	.dump		= cake_dump,
	.dump_stats	= cake_dump_stats,
	.owner		= THIS_MODULE,
};

static int __init cake_module_init(void)
{
	return register_qdisc(&cake_qdisc_ops);
}

static void __exit cake_module_exit(void)
{
	unregister_qdisc(&cake_qdisc_ops);
}

module_init(cake_module_init);
module_exit(cake_module_exit);

MODULE_AUTHOR("Jonathan Morton, Toke Høiland-Jørgensen, Dave Täht, Kevin Darbyshire-Bryant");
MODULE_DESCRIPTION("Common Applications Kept Enhanced (CAKE) Qdisc");
MODULE_LICENSE("Dual BSD/GPL");
