/*
 * microSD bring-up (plan Phase 1 item 3): mount, sequential speed (ROM load
 * time), write + fsync latency, long file names, and whether rename() can
 * replace an existing file (the crash-safe write pattern of plan D6 needs
 * that).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#include "hwtest_priv.h"
#include "retro_time.h"

#define DIR_PATH RETRO_TAB5_SD_MOUNT "/retro/hwtest"
#define SPEED_FILE DIR_PATH "/speed.bin"
/* Small enough to finish quickly on a badly formatted card (512-byte
 * clusters run at ~1 MB/s). */
#define SPEED_BYTES (4u << 20)
#define CHUNK (64u << 10)
#define LFN_NAME "Long File Name Test - The Legend of Zelda (USA) (Rev 1) [!].nes"

static float mbps(size_t bytes, uint64_t us)
{
    return us ? (float)bytes / (float)us : 0;
}

static bool write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    bool ok = fputs(text, f) >= 0;
    return (fclose(f) == 0) && ok;
}

static void speed_test(void)
{
    /* Cache-line aligned, or the SDMMC driver bounces every sector through
     * its own buffer (~1 MB/s). */
    uint8_t *buf = heap_caps_aligned_alloc(64, CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *rom = heap_caps_aligned_alloc(128, SPEED_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf || !rom) {
        hw_result(HW_FAIL, "sd.speed", "out of memory");
        goto out;
    }
    memset(buf, 0xA5, CHUNK);

    int fd = open(SPEED_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        hw_result(HW_FAIL, "sd.write", "open failed: %s", strerror(errno));
        goto out;
    }
    uint64_t t0 = retro_time_us();
    size_t done = 0;
    while (done < SPEED_BYTES && write(fd, buf, CHUNK) == (ssize_t)CHUNK) {
        done += CHUNK;
    }
    fsync(fd);
    close(fd);
    uint64_t w_us = retro_time_us() - t0;
    if (done < SPEED_BYTES) {
        hw_result(HW_FAIL, "sd.write", "short write (%u of %u bytes)", (unsigned)done,
                  (unsigned)SPEED_BYTES);
        goto out;
    }

    /* Read into internal RAM (DMA straight to the buffer) at a few request
     * sizes... */
    fd = open(SPEED_FILE, O_RDONLY);
    static const size_t req[] = {4096, 16384, CHUNK};
    char per_size[64];
    size_t plen = 0;
    ssize_t n;
    uint64_t r_us = 0;
    for (size_t i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
        lseek(fd, 0, SEEK_SET);
        t0 = retro_time_us();
        done = 0;
        while (done < SPEED_BYTES / 4 && (n = read(fd, buf, req[i])) > 0) {
            done += (size_t)n;
        }
        r_us = retro_time_us() - t0;
        plen += snprintf(per_size + plen, sizeof(per_size) - plen, "%s%u KB: %.1f", i ? ", " : "",
                         (unsigned)(req[i] >> 10), (double)mbps(done, r_us));
    }
    hw_result(HW_INFO, "sd.fat_read_mbps", "%s MB/s", per_size);
    /* ...and into PSRAM, the way a ROM gets loaded. */
    lseek(fd, 0, SEEK_SET);
    t0 = retro_time_us();
    size_t rom_done = 0;
    while (rom_done < SPEED_BYTES && (n = read(fd, rom + rom_done, CHUNK)) > 0) {
        rom_done += (size_t)n;
    }
    uint64_t rom_us = retro_time_us() - t0;
    close(fd);

    float rom_mbps = mbps(rom_done, rom_us);
    hw_result(HW_INFO, "sd.seq_mbps", "write %.1f, read %.1f (SRAM), %.1f (PSRAM) MB/s",
              (double)mbps(SPEED_BYTES, w_us), (double)mbps(done, r_us), (double)rom_mbps);
    hw_result(HW_INFO, "sd.rom_load_ms", "%.0f ms for a 4 MB ROM into PSRAM", hw_ms(rom_us));
    unlink(SPEED_FILE);

out:
    heap_caps_free(buf);
    heap_caps_free(rom);
}

/* The bus without FAT: 4 MB at a few request sizes, from sector 0x10000
 * (32 MB in, clear of the FAT tables). */
static void raw_read_test(void)
{
    uint8_t *buf = heap_caps_aligned_alloc(64, CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        return;
    }
    static const size_t sizes[] = {4096, 16384, CHUNK};
    char msg[96];
    size_t len = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t per = sizes[i] / 512, total = (4u << 20) / 512;
        uint64_t t0 = retro_time_us();
        bool ok = true;
        for (size_t s = 0; s < total && ok; s += per) {
            ok = retro_tab5_sd_read_sectors(buf, 0x10000 + (uint32_t)s, per);
        }
        uint64_t us = retro_time_us() - t0;
        len += snprintf(msg + len, sizeof(msg) - len, "%s%u KB: %s", i ? ", " : "",
                        (unsigned)(sizes[i] >> 10), ok ? "" : "ERR");
        if (ok) {
            len += snprintf(msg + len, sizeof(msg) - len, "%.1f", (double)mbps(4u << 20, us));
        }
    }
    hw_result(HW_INFO, "sd.raw_read_mbps", "%s MB/s", msg);
    heap_caps_free(buf);
}

static void fsync_test(void)
{
    int fd = open(DIR_PATH "/fsync.bin", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        hw_result(HW_FAIL, "sd.fsync", "open failed: %s", strerror(errno));
        return;
    }
    char block[512];
    memset(block, 'S', sizeof(block));
    uint64_t total = 0, worst = 0;
    const int n = 20;
    for (int i = 0; i < n; i++) {
        uint64_t t0 = retro_time_us();
        write(fd, block, sizeof(block));
        fsync(fd);
        uint64_t d = retro_time_us() - t0;
        total += d;
        worst = d > worst ? d : worst;
    }
    close(fd);
    unlink(DIR_PATH "/fsync.bin");
    hw_result(HW_INFO, "sd.fsync_ms", "512 B write + fsync: avg %.1f, max %.1f ms",
              hw_ms(total / n), hw_ms(worst));
}

static void lfn_test(void)
{
    const char *path = DIR_PATH "/" LFN_NAME;
    bool found = false;
    if (write_file(path, "lfn")) {
        DIR *d = opendir(DIR_PATH);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            found |= strcmp(e->d_name, LFN_NAME) == 0;
        }
        if (d) {
            closedir(d);
        }
        unlink(path);
    }
    hw_result(found ? HW_PASS : HW_FAIL, "sd.long_names", "%s",
              found ? "63-char name round-trips" : "long name not preserved");
}

static void rename_test(void)
{
    const char *tmp = DIR_PATH "/save.tmp";
    const char *dst = DIR_PATH "/save.srm";
    bool ok = write_file(dst, "old") && write_file(tmp, "new");
    if (!ok) {
        hw_result(HW_FAIL, "sd.rename_replace", "couldn't create test files");
        return;
    }
    if (rename(tmp, dst) == 0) {
        hw_result(HW_PASS, "sd.rename_replace", "rename() replaces an existing file");
    } else {
        int e = errno;
        /* D6 then needs unlink + rename, with recovery of a leftover .tmp at
         * boot if power dies between the two. */
        bool fallback = unlink(dst) == 0 && rename(tmp, dst) == 0;
        hw_result(HW_WARN, "sd.rename_replace", "rename() over a file fails (%s); unlink+rename %s",
                  strerror(e), fallback ? "works" : "also fails");
    }
    unlink(tmp);
    unlink(dst);
}

void hwtest_sd(void)
{
    if (!retro_tab5_sd_mounted() && !retro_tab5_sd_mount()) {
        hw_result(HW_FAIL, "sd.mount", "no card, or not FAT (never formatted by hwtest)");
        return;
    }
    retro_tab5_sd_info_t info;
    if (retro_tab5_sd_info(&info)) {
        hw_result(HW_PASS, "sd.mount", "\"%s\" %.1f GB %s, %u-byte clusters, %u-bit @ %u kHz",
                  info.name, (double)info.capacity_bytes / 1e9, info.fs_type, info.cluster_bytes,
                  info.bus_width, info.freq_khz);
        /* FatFs reads at most one cluster per request, so small clusters
         * cap throughput no matter how fast the bus is. */
        if (info.cluster_bytes && info.cluster_bytes < 16384) {
            hw_result(HW_WARN, "sd.cluster_size", "%u-byte clusters make FAT I/O slow; format with 32 KB",
                      info.cluster_bytes);
        }
    }
    mkdir(RETRO_TAB5_SD_MOUNT "/retro", 0775);
    mkdir(DIR_PATH, 0775);

    raw_read_test();
    speed_test();
    fsync_test();
    lfn_test();
    rename_test();
}
