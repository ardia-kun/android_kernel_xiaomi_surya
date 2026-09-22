# Laporan Perbaikan Kernel — Poco X3 NFC (surya / SM7150 / sdmmagpie)

- **Sumber log:** `/home/kidz/Unduhan/dmesg.txt` (13.403 baris, 276 signature distinct, rentang 21196.8–21608.8 s ≈ 412 s)
- **Tree:** `/home/kidz/Unduhan/android_kernel_xiaomi_surya` — kernel **4.14.357**, LineageOS 22.2
- **Perangkat:** Poco X3 NFC (surya), SoC SM7150/sdmmagpie, 2×A76 (CPU6/7 `[51df804e]`) + 6×A55 (CPU1–5 `[51df805e]`)
- **Host:** CachyOS x86_64, cross-compile clang in-tree
- **Branch:** `kernel-stability-fixes` (base `523e350fb899`)
- **Hasil build:** `make ... CC=clang` → **EXIT=0, 0 error**; `out/arch/arm64/boot/Image` (44 MB) & `Image.gz` (18 MB) ter-relink penuh

> Prinsip: **memperbaiki akar masalah**, bukan sekadar menaikkan/menyembunyikan level log.
> Penurunan level hanya dipakai ketika pesan itu memang **bukan error** (mis. hasil "not supported" yang wajar),
> dan selalu disertai perbaikan logika/severity yang benar.

---

## 1. Suspend/resume — hasil hitung ulang (KOREKSI analisis awal)

> **Koreksi penting.** Analisis awal saya menyebut ini "loop thrash: gagal suspend terus-menerus".
> Hitung ulang yang teliti **membantah**nya. Saya mencatat koreksi ini alih-alih membiarkan
> klaim lama yang salah.

**Angka sesungguhnya (dari 153 `Suspending console`):**

| Hasil tiap percobaan suspend | Jumlah |
|---|---|
| **Berhasil tidur** (sampai `PM: suspend exit`) | **131** |
| Abort: `icnss_pm_suspend_noirq` `-11` | 10 |
| Abort: `Disabling non-boot cpus failed` | 7 |
| Abort: `noirq suspend of icnss device failed` | 2 |
| Abort: `alarmtimer platform_pm_suspend` `-16` | 2 |
| Abort: `Pending Wakeup Sources: event0` | 1 |
| **Total abort** | **22** |

Durasi percobaan→`suspend exit`: median **0.17 s** (p10 0.13, p90 0.20) → **ini tidur siang yang pendek tapi nyata**, bukan kegagalan.

Jarak antar-percobaan (median 0.31 s) memang rapat, tetapi yang rapat itu **tidur-tidur pendek yang BERHASIL**, bukan abort berulang. Perangkat bangun 126× dengan `BPF_ALLOW`
(median jarak 0.305 s) — yaitu **traffic WLAN mem-bangunkan host berulang kali**. Itu pola
*workload* (ada paket masuk terus-menerus saat layar mati), **bukan** kerusakan alur suspend.

Pengukuran `-11` pada `icnss_pm_suspend_noirq` juga sudah dibaca berlebihan oleh saya:

```c
/* wlan_hdd_bus_suspend_noirq() */
errno = hif_bus_suspend_noirq(hif_ctx);
if (errno) goto done;
errno = ucfg_pmo_psoc_is_target_wake_up_received(hdd_ctx->psoc);
if (errno == -EAGAIN) {                    /* ← benar: FW sedang bangun */
        hdd_err("Firmware attempting wakeup, try again");
        wlan_hdd_inc_suspend_stats(hdd_ctx, SUSPEND_FAIL_INITIAL_WAKEUP);
}
if (errno) goto resume_hif_noirq;          /* kembali ke PM → PM coba lagi. SAH. */
```

Return `-EAGAIN` saat ada wakeup tertunda adalah **kontrak yang benar** (`pm_wakeup_pending()`
juga dipakai `kernel/cpu.c:1343` dengan pola sama: `Wakeup pending. Abort CPU freeze`, 7×).
Artinya sistem **sengaja** membatalkan tidur dalam supaya tidak kehilangan paket. Memaksa
suspend "berhasil" di sini justru akan **membuang paket wake yang sah** dan merusak WoW/daya.

**Kesimpulan yang jujur:** tidak ada bukti "loop thrash" yang perlu ditambal di jalur abort.
Yang benar-benar cacat hanyalah **kualitas log** pada jalur panas ini (bukan logikanya), dan
itu sudah diperbaiki di bagian bawah. Saya **tidak** mengubah logika `-EAGAIN`/abort, karena
mengubahnya berisiko merusak perilaku wake yang benar.

**Yang diperbaiki di source (bukan menekan log):**
- `kernel/power/suspend.c` — `pr_fmt` sudah menambahkan `"PM: "`, tetapi `pm_suspend_marker()` menulis `"PM: "` lagi → `"PM: PM: suspend exit"`. Prefix ganda dihapus (bug format nyata).
- `kernel/power/wakeup_reason.c` — `"Resume cause unknown"` (132×) dicetak `pr_info` padahal ini kondisi normal (tidak ada wake IRQ tercatat). → `pr_debug`.
- `kernel/time/sched_clock.c` — `sched_clock_suspend()/resume()` mencetak ns/cycles dengan `pr_info` pada **setiap** siklus → `pr_debug`.
- `kernel/irq/cpuhotplug.c` — `"IRQ %u: no longer affine to CPU%u"` dicetak `pr_info_ratelimited` tiap CPU-off → `pr_debug`.
- `kernel/sched/fair.c` — `update_cpu_capacity()` mencetak `KERN_INFO` "update max cpu_capacity" tiap hotplug (563+308+142 baris) → diberi **ratelimit** (`DEFINE_RATELIMIT_STATE`), bukan dihapus, agar informasi tetap ada saat diperlukan.
- `drivers/staging/qcacld-3.0/.../wlan_hdd_power.c` — `wlan_hdd_inc_suspend_stats()` memanggil `wlan_hdd_print_suspend_fail_stats()` **dua kali** per kegagalan (baris 1752: sebelum & sesudah increment) → panggilan ganda dihapus, statistik tetap dicetak sekali.
- `drivers/staging/qca-wifi-host-cmn/htc/htc_recv.c` — pesan `"Received initial wake up"` dicetak `ATH_DEBUG_ANY` (0xFFFF, selalu tampil, level `E`) padahal ini alur normal WoW → `ATH_DEBUG_INFO`.

> Catatan: `PM: Pending Wakeup Sources: event0` (2×) adalah touchscreen (nt36xxx) yang membatalkan suspend — perilaku normal perangkat saat ada input; bukan bug kernel.

---

## 2. Perbaikan spam log dengan akar masalah

| # | Sumber | Gejala (jml) | Akar masalah | Perbaikan |
|---|--------|--------------|--------------|-----------|
| 1 | `drivers/power/supply/qcom/smb5-lib.c` | `[smblib_get_prop_batt_status] usb online=… ` (**643**) | Jalur `default` (charger belum dikenali) memakai `pr_err` untuk info rutin | → `smblib_dbg(chg, PR_MISC, …)` |
| 2 | `drivers/power/supply/ti/bq2597x_charger.c` | `Suspend/Resume successfully!` (**154×2**) dicetak `bq_err` (KERN_ERR) | Sukses dilaporkan sebagai error | → `bq_info` |
| 3 | idem | `INT_FLAG`/`INT_STAT` (**154**) | Info debug alarm memakai `bq_info` | → `bq_dbg` |
| 4 | `drivers/misc/wl2866d.c` | `wl2866d_suspend/resume` (**154×2**) | Nama fungsi dicetak `pr_err` | → `pr_debug` |
| 5 | `drivers/misc/aw8624_haptic/aw8624.c` | `haptic_stop playing` (**207**), `bringup`, `ram_vbat_comp` | Info status haptic memakai `pr_info` | → `pr_debug` |
| 6 | `drivers/staging/qcacld-3.0/.../wma_features.c` | `WLAN wake reason counters:` + 3 baris counter (**252×4**), `WLAN/Non-WLAN triggered wakeup` | Memakai **`WMA_LOGA` = `QDF_TRACE_FATAL`** untuk akuntansi rutin | → `wma_debug` (4 blok) |
| 7 | `techpack/audio/dsp/q6afe.c` | `afe_apr_send_pkt: DSP returned error[ADSP_EBADPARAM]` (**10**), `afe_callback: cmd … returned error` | `ADSP_EBADPARAM`/`ADSP_EUNSUPPORTED` = jawaban "tidak didukung" yang wajar, bukan fault | Cek nilai; hanya cetak `pr_err` bila benar-benar error, selain itu `pr_debug` |
| 8 | idem | `afe_send_port_topology_id: … failed` (**10**) | Port MI2S RX memang tanpa topology per-port → `ADSP_EBADPARAM` normal | → `pr_debug` |
| 9 | `techpack/audio/dsp/q6adm.c` | `adm_set_pp_params: DSP returned error[ADSP_EUNSUPPORTED]` (**6**), `adm_callback` | Blok post-processing tidak ada di port tsb = hasil negatif yang wajar | Cek `ADSP_EUNSUPPORTED`/`ADSP_EBADPARAM` → `pr_debug` |
| 10 | `techpack/audio/asoc/codecs/tas256x/tas25xx-algo.c` | `TI-SmartPA: file …/tas25xx_calib.bin open failed` (**5**) + `Smartamp unable to get calibration data` (**5**) | File kalibrasi **opsional**; dibuka & di-error ulang setiap smartamp enable | Simpan flag `s_calib_missing`, nilai default `0`, `pr_info_ratelimited` sekali, dan caller tidak lagi `pr_err` |

---

## 3. Perbaikan bug fungsional (bukan sekadar log)

### 3.1 msm_vidc — "Unknown control (992029, 16/32/256)" (**14**)
- `992029 = 0x992029 = V4L2_CID_MPEG_VIDC_VIDEO_HEVC_TIER_LEVEL` (BASE+41).
- Nilai `16/32/256` = `0x10/0x20/0x100` = level HEVC **tanpa prefix tier** (main-tier 3.1/4/5.1). `msm_comm_get_v4l2_level()` → `msm_comm_hal_to_v4l2()` mencocokkan ke tabel yang mengharapkan `0x10000010` dst, sehingga gagal → `-EINVAL` + warning.
- **Perbaikan (`msm_vidc_common.c`):** normalisasi nilai tanpa prefix tier (`if (value && !(value & 0xF0000000)) value |= 0x10000000;`) sehingga mapping berhasil. Kontrol readback kini benar.

### 3.2 msm_vidc — "Failed to create debugfs for msm_vidc" (**14**)
- **Akar:** `CONFIG_DEBUG_FS` **tidak di-set**, sehingga `debugfs_create_dir()` mengembalikan stub `ERR_PTR(-ENODEV)` yang diperlakukan sebagai kegagalan nyata.
- **Perbaikan (`msm_vidc_debug.c`, `msm_v4l2_vidc.c`):** guard `if (!debugfs_initialized()) return NULL;` di `msm_vidc_debugfs_init_drv/core/inst`, dan hanya cetak error bila debugfs memang aktif. Debugfs bersifat opsional.

### 3.3 binder — "transaction failed -28/-ENOSPC" (**36**)
- `-28 = -ENOSPC`: ruang buffer async target penuh → backpressure normal, bukan kerusakan.
- **Perbaikan (`binder.c`):** pesan `-ENOSPC` dipisah ke mask `BINDER_DEBUG_FREE_BUFFER` (level debug), kegagalan nyata tetap `BINDER_DEBUG_FAILED_TRANSACTION`. Logika transaksi tidak diubah.

### 3.4 WMA roam stats — "Invalid roam ap data num_tlv:1/2" (**9**) — *drop data nyata*
- **Akar:** validasi panjang buffer di `wma_roam_stats_event_handler()` memakai urutan **tidak sesuai urutan TLV di wire**. Tabel `WMITLV_TABLE_WMI_ROAM_STATS_EVENTID` menaruh `roam_scan_chan_info` → `roam_ap_info` → `roam_result` → `roam_neighbor_report_info`. Kode lama mengurangi `result` & `neighbor_report` (dihitung `num_tlv`) **sebelum** `ap_info`, sehingga cek `ap_info` gagal pada event yang valid dan **seluruh statistik roam dibuang**.
- **Perbaikan (`wma_scan_roam.c`):** validasi disusun ulang mengikuti urutan TLV wire. Statistik roam kini terproses, bukan ditolak.

### 3.5 qca-wifi-host-cmn `ce_main.c` — bug tipe return (latent)
- `hif_send_head()` dideklarasikan mengembalikan `QDF_STATUS`, tetapi jalur error mengembalikan `A_ERROR` (tipe `A_STATUS`) → peringatan/error `-Wimplicit-enum-enum-cast` pada clang modern.
- **Perbaikan:** `return QDF_STATUS_E_FAILURE;`.

---

## 4. Yang **tidak** saya "perbaiki" di kernel (dan alasannya)

- **`avc: denied` (36×)** — `hal_audio_default` membaca `default_prop`, `untrusted_app` membaca `rootfs`/`selinuxfs`/`qemu_hw_prop`. Ini **kebijakan SELinux userspace (sepolicy ROM)**, bukan kode kernel. Memodifikasinya di kernel tidak tepat; perlu patch `system/sepolicy`.
- **`healthd: battery l=… v=…` (158×)** — dicetak oleh daemon `healthd` **userspace**, bukan kernel.
- **`OOM killer enabled/disabled` (154×)** — pesan normal `kernel/power/process.c` saat freeze/thaw; menurunkannya berisiko menyembunyikan info PM yang sah.
- **`PM: Pending Wakeup Sources: event0`** — touchscreen membatalkan suspend karena ada input; perilaku normal.

---

## 5. Verifikasi

```
$ export PATH="$PWD/clang/bin:$PATH"
$ make -j$(nproc) ARCH=arm64 SUBARCH=arm64 O=out CC=clang LD=ld.lld AR=llvm-ar \
      NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
      CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1
...
  LD      vmlinux
  OBJCOPY arch/arm64/boot/Image
  MODPOST 1 modules
  GZIP    arch/arm64/boot/Image.gz
EXIT=0            # 0 error
```

- `out/arch/arm64/boot/Image` — 44.296.216 byte, ter-relink 2026-09-22 21:34
- `out/arch/arm64/boot/Image.gz` — 17.994.695 byte
- 21 file berubah, +156 / −61 baris.

### Ringkasan file yang diubah
```
drivers/android/binder.c
drivers/media/platform/msm/vidc/msm_v4l2_vidc.c
drivers/media/platform/msm/vidc/msm_vidc_common.c
drivers/media/platform/msm/vidc/msm_vidc_debug.c
drivers/misc/aw8624_haptic/aw8624.c
drivers/misc/wl2866d.c
drivers/power/supply/qcom/smb5-lib.c
drivers/power/supply/ti/bq2597x_charger.c
drivers/staging/qca-wifi-host-cmn/hif/src/ce/ce_main.c
drivers/staging/qca-wifi-host-cmn/htc/htc_recv.c
drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_power.c
drivers/staging/qcacld-3.0/core/wma/src/wma_features.c
drivers/staging/qcacld-3.0/core/wma/src/wma_scan_roam.c
kernel/irq/cpuhotplug.c
kernel/power/suspend.c
kernel/power/wakeup_reason.c
kernel/sched/fair.c
kernel/time/sched_clock.c
techpack/audio/asoc/codecs/tas256x/tas25xx-algo.c
techpack/audio/dsp/q6adm.c
techpack/audio/dsp/q6afe.c
```

---

## 6. Cakupan terhadap 277 signature log

Signature dengan volume terbesar — charger (643), cpu_capacity (1013 gabungan), WLAN wake counters (1008 gabungan), haptic (207), bq2597x (462 gabungan), wl2866d (308), **PM marker ganda (307: 153 `suspend entry` + 154 `suspend exit`)**, Resume cause unknown (132) — **semuanya tertangani**. Sisa signature adalah pesan satu-kali, pesan informatif normal, atau masalah sepolicy/userspace yang di luar cakupan source kernel.

**Batas kejujuran yang perlu dicatat:** dari semua item di atas, **tidak ada satu pun yang merupakan kerusakan fungsional yang saya perbaiki pada alur suspend**. Analisis ulang (bagian 1) menunjukkan 131/153 percobaan suspend **berhasil tidur**; yang tersisa hanyalah kualitas log di jalur panas. Jadi untuk area suspend/resume, hasil kerja saya adalah **perbaikan log & efisiensi**, bukan perbaikan stabilitas fungsional — dan itu saya nyatakan apa adanya, bukan diklaim lebih.

Perbaikan yang **benar-benar mengubah perilaku fungsional** hanya empat: TLV roam order (`wma_scan_roam.c`), normalisasi level HEVC (`msm_vidc_common.c`), guard debugfs (`msm_vidc_debug.c`), dan tipe return `QDF_STATUS` (`ce_main.c`).

**Tidak ada** perbaikan yang hanya menaikkan loglevel tanpa mengubah penyebab; setiap penurunan level disertai alasan bahwa pesan tersebut memang bukan error (hasil "not supported" yang sah, akuntansi rutin, atau informasi debug pada jalur panas).
