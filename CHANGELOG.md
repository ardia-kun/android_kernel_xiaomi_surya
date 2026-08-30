# Changelog - underclock-undervolt

## v6.0.0 (2026-08-23)

### Scheduler & Power
- BORE Scheduler 5.1.0 (Balanced Optimization for Responsive Execution)
- BORE recalibrate tuning for optimal balance
- WALT governor power efficiency optimizations
- reweight_task backport (sched/fair)
- TEO cpuidle governor: util-awareness (thermal pressure)
- Iowait boost restriction capped at 50% max_freq

### CPU Underclock & Undervolt
- 3-cluster CPU frequency scaling (L2/MSM/CPR)
- Per-cluster voltage offset control via sysfs
- 300MHz (OSM_INIT_RATE) clock fix for core_count filter

### GPU Tuning
- GPU underclock support (267-825MHz, 8 pwrlevels)
- L2PC PM QoS removed for lower idle power
- Lock-less page pool for better memory efficiency
- Disable adreno snapshot, coresight, trace (smaller Image)
- DMA API conversions, locking improvements, fence fixes
- GPU frequency reporting fixes

### Thermal Management
- Custom step-wise cooling policy for sdmmagpie
- Full thermal zone mapping (CPU silver 6 + gold 4 + GPU + AOSS + CPUSS + LMH)
- Energy costs & scheduler thermal margins backport

### Memory Compression (zswap/zram/zsmalloc)
- **zswap**: Single pool architecture, exclusive loads, writeback race fix, shrink mechanism, z3fold deprecation
- **zram**: Multi-zcomp recompression, idle tracking, age-based idle interface, incompressible page stats, writeback support
- **zsmalloc**: Fullness grouping, compaction rework, chain size optimization, memory hygiene
- **lzo-rle**: Run-length encoding (default compressor)
- **lib/lz4**: v1.10.0 + ARM64 ASM acceleration (15 commits)

### Binder IPC
- Cluster-aware binder process selection
- Cgroup-aware task migration for wakelocks

### Wakeup & Wakelock
- Boeffla Wakelock Blocker v1.1.0
- Wakelock Governor v1.0 (le9ec working set protection)

### KernelSU
- KernelSU import (32590) with hostsredirect
- SYS_faccessat sdcardfs tweak
- do_faccessat() helper + faccessat2 syscall
- vfs access_override_creds
- seccomp filter count reporting

### Drivers & Misc
- Bluetooth: check key sizes only when SSP enabled
- selinux: remove audit dependency
- net: TTL/HL preservation
- kernel: bypass modversion disagreements
- kernel: do not build modules.order
- Dynamic RAM (XMP) & DDR Bus Frequency Reporting
- Deprecate Core_CTL and MSM_Performance Hotplug

### Build Optimization (size-optimize branch)
- DEBUG_INFO=n, CC_OPTIMIZE_FOR_SIZE=y
- IKCONFIG=n, TRACING=n, FTRACE=n, PROFILING=n
- PERF_EVENTS=y (ARM_MEMLAT_MON dependency)
- Image size: ~35MB
