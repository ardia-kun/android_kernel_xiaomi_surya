===============
EEVDF Scheduler
===============

The "Earliest Eligible Virtual Deadline First" (EEVDF) was first introduced
in a scientific publication in 1995 [1]. This kernel includes an EEVDF-inspired
adaptation of the CFS scheduler, backported from the concepts used in the
upstream Linux 6.6+ implementation by Peter Zijlstra [2-4].

Overview
========

This implementation adds EEVDF-inspired improvements to the existing CFS
scheduler without requiring the full infrastructure changes (augmented
rb-tree, etc.) that the upstream implementation uses.

Key Concepts
============

Virtual Runtime (vruntime)
  Same as CFS -- tracks CPU time consumed by a task, normalized by weight.

Eligibility
  An entity is "eligible" if it has not exceeded its fair share of CPU time.
  Approximated by checking if vruntime <= min_vruntime + tolerance.

Virtual Deadline
  Computed as: deadline = vruntime + calc_delta_fair(slice, se)
  Tasks with shorter slices get earlier deadlines and are prioritized.

Slice
  The base time slice for deadline computation. Configurable via
  /proc/sys/kernel/sched_base_slice_ns (default: 3ms).

Task Selection
==============

When the EEVDF feature is enabled (CONFIG_SCHED_EEVDF=y), pick_next_entity()
uses the following algorithm:

1. Walk up to EEVDF_MAX_CANDIDATES (8) entities from the leftmost node
2. Filter for eligible entities (vruntime not too far ahead of min_vruntime)
3. Among eligible entities, pick the one with the earliest virtual deadline
4. Fall back to standard CFS leftmost selection if no eligible entity found

Preemption
==========

Tick preemption: A running entity is preempted when its vruntime passes
its virtual deadline (slice exhausted in virtual time).

Wakeup preemption: A waking entity preempts if it is eligible AND has
an earlier virtual deadline than the current entity.

Configuration
=============

The EEVDF behavior can be controlled at runtime:

- /sys/kernel/debug/sched_features: Toggle "EEVDF" on/off
- /proc/sys/kernel/sched_base_slice_ns: Adjust base time slice

  Recommended values:
  - 1000000 (1ms): Very responsive, higher overhead
  - 3000000 (3ms): Good balance (default)
  - 6000000 (6ms): Better throughput, slightly higher latency

References
==========

[1] I. Stoica and H. Abdel-Wahab, "Earliest Eligible Virtual Deadline
    First: A Flexible and Accurate Mechanism for Proportional Share
    Resource Allocation", 1995.
    http://www.cs.berkeley.edu/~istoica/papers/eevdf-tr-95.pdf

[2] Peter Zijlstra, "sched/fair: Implement an EEVDF like policy", 2023.

[3] Linux 6.6 merge: EEVDF scheduler replacement for CFS.

[4] https://docs.kernel.org/scheduler/sched-eevdf.html
