/**
 * @file    vol_image.c
 * @brief   Bridge between the device's USB volume and real FAT tools.
 *
 *   vol_image dump  <image> [big]   build a device with sample data and write
 *                                   its whole volume out as a disk image
 *   vol_image apply <image> [big]   build the same device, then play every
 *                                   sector that differs in <image> back into
 *                                   usbs_write(), end the session, and print
 *                                   what the device did
 *
 * With mtools and dosfstools this lets a real FAT implementation (not the
 * test suite's own host model) mount the volume, copy a file over
 * SETTINGS.CSV, and have fsck.fat judge the result. See fs_check.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_cfg.h"
#include "session.h"
#include "settings_file.h"
#include "log_store.h"
#include "record_buffer.h"
#include "timeutil.h"
#include "usb_storage.h"
#include "platform_if.h"

extern void host_flash_erase_all(void);
extern void host_set_time(const app_datetime_t *dt);

static log_store_t     g_ls;
static record_buffer_t g_rb;
static device_cfg_t    g_cfg;

static const app_datetime_t k_now = { 2026u, 10u, 6u, 9u, 30u, 0u };

/* The cards the unit has been given: some of the ones that tap, one that never does. */
static const uint32_t k_cards[] = { 1000u, 1007u, 1014u, 1021u, 1028u, 5000u, 777777u };

static bool next_card(void *ctx, uint32_t *id)
{
    uint32_t *i = (uint32_t *)ctx;

    if (*i >= (sizeof(k_cards) / sizeof(k_cards[0]))) {
        return false;
    }
    *id = k_cards[(*i)++];
    return true;
}

static void push_marker(app_epoch_t start, const char *module, const char *lecture)
{
    app_record_t m[SESS_MAX_RECORDS];
    uint16_t k, c = sess_encode(m, SESS_MAX_RECORDS, start, module, lecture);

    for (k = 0u; k < c; k++) {
        (void)rb_push(&g_rb, &m[k]);
    }
}

/* @p second_lecture: a lecture started from the button at record 45, named
 * the way the firmware names it ("Lecture 1" -> "Lecture 2"). */
static void setup(uint32_t n_records, bool second_lecture)
{
    uint32_t i;

    host_flash_erase_all();
    host_set_time(&k_now);
    {
        uint32_t i2 = 0u, crc = CARDS_CRC_INIT;
        const uint32_t n = (uint32_t)(sizeof(k_cards) / sizeof(k_cards[0]));

        for (i2 = 0u; i2 < n; i2++) {
            crc = cards_crc_update(crc, k_cards[i2]);
        }
        i2 = 0u;
        (void)devcfg_set_cards(&g_cfg, 0xC0FFEEu, n, cards_crc_final(crc), next_card, &i2);
    }

    log_init(&g_ls);
    rb_init(&g_rb);
    for (i = 0u; i < n_records; i++) {
        app_record_t rec;

        if (i == 30u) {
            /* A lecture starts here: records 30.. belong to it. */
            push_marker(time_to_epoch(&k_now) + (i * 37u), "EN2090", "Lecture 1");
        }
        if (i == 45u && second_lecture) {
            char next[SESS_LECTURE_MAX + 1u];

            sess_next_name("Lecture 1", next);
            push_marker(time_to_epoch(&k_now) + (i * 37u), "EN2090", next);
        }
        rec.student_id = 1000u + (7u * (i % 5u));
        rec.stamp = time_to_epoch(&k_now) + (i * 37u);
        rb_push(&g_rb, &rec);
        if (rb_needs_flush(&g_rb)) {
            (void)log_flush(&g_ls, &g_rb);
        }
    }
    (void)log_flush(&g_ls, &g_rb);

    usbs_begin(&g_ls, &g_cfg, &k_now);
}

int main(int argc, char **argv)
{
    uint32_t total, lba;
    uint8_t sec[512], base[512];
    FILE *f;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s dump|apply|files image-or-dir [big|lectures]\n", argv[0]);
        return 2;
    }
    /* A third argument "big" fills the log to near capacity; "lectures" adds a
     * second lecture, started from the button, at record 45. */
    setup((argc == 4 && strcmp(argv[3], "big") == 0) ? 13900u : 60u,
          argc == 4 && strcmp(argv[3], "lectures") == 0);
    total = usbs_sector_count();

    if (strcmp(argv[1], "dump") == 0) {
        f = fopen(argv[2], "wb");
        if (f == NULL) { perror("open"); return 2; }
        for (lba = 0u; lba < total; lba++) {
            if (!usbs_read(lba, sec, 1u)) { fprintf(stderr, "read %u failed\n", lba); return 1; }
            fwrite(sec, 1u, 512u, f);
        }
        fclose(f);
        printf("dumped %u sectors\n", total);
        return 0;
    }

    /* "files <dir>": write ATTEND.CSV, SETTINGS.CSV, STATUS.TXT and LECTURES.CSV exactly as
     * the device presents them, by reading the volume the way a host would. */
    if (strcmp(argv[1], "files") == 0) {
        uint8_t root[512], fat[FAT12_FAT_BYTES];
        uint32_t e, i;

        (void)usbs_read(FAT12_ROOT_START_LBA, root, 1u);
        for (i = 0u; i < FAT12_SECTORS_PER_FAT; i++) {
            (void)usbs_read(FAT12_FAT_START_LBA + i, &fat[i * 512u], 1u);
        }
        for (e = 0u; e < FAT12_ROOT_ENTRIES; e++) {
            const uint8_t *d = &root[e * 32u];
            char name[16], path[1024];
            uint32_t n = 0u, size, cluster, done = 0u;

            if (d[0] == 0x00u) { break; }
            if (d[0] == 0xE5u || (d[11] & FAT12_ATTR_VOLUME_ID) != 0u) { continue; }
            for (i = 0u; i < 8u && d[i] != ' '; i++) { name[n++] = (char)d[i]; }
            name[n++] = '.';
            for (i = 8u; i < 11u && d[i] != ' '; i++) { name[n++] = (char)d[i]; }
            name[n] = '\0';
            size = fat12_rd32(&d[28]);
            cluster = fat12_rd16(&d[26]);
            snprintf(path, sizeof(path), "%s/%s", argv[2], name);
            f = fopen(path, "wb");
            if (f == NULL) { perror(path); return 2; }
            while (done < size && cluster >= 2u && cluster < 0xFF8u) {
                uint32_t chunk = size - done;

                (void)usbs_read(fat12_cluster_lba(cluster), sec, 1u);
                fwrite(sec, 1u, (chunk > 512u) ? 512u : chunk, f);
                done += 512u;
                cluster = fat12_get(fat, cluster);
            }
            fclose(f);
            printf("wrote %s (%u bytes)\n", name, size);
        }
        return 0;
    }

    if (strcmp(argv[1], "apply") == 0) {
        usbs_result_t r;
        uint32_t changed = 0u, refused = 0u;
        device_cfg_t cfg;
        app_datetime_t now;

        /* The host's picture of the disk is the pristine volume it was shown.
         * Take it before replaying anything: STATUS.TXT is generated from live
         * state, so reading it back mid-replay would differ from what the host
         * actually saw, and a real host never writes sectors it did not change. */
        uint8_t *pristine = (uint8_t *)malloc((size_t)total * 512u);

        if (pristine == NULL) { return 2; }
        for (lba = 0u; lba < total; lba++) {
            (void)usbs_read(lba, &pristine[(size_t)lba * 512u], 1u);
        }

        f = fopen(argv[2], "rb");
        if (f == NULL) { perror("open"); return 2; }
        for (lba = 0u; lba < total; lba++) {
            if (fread(sec, 1u, 512u, f) != 512u) { fprintf(stderr, "image too short at %u\n", lba); return 1; }
            memcpy(base, &pristine[(size_t)lba * 512u], 512u);
            if (memcmp(sec, base, 512u) != 0) {
                changed++;
                if (!usbs_write(lba, sec, 1u)) {
                    refused++;
                    printf("write to LBA %u refused\n", lba);
                }
            }
        }
        fclose(f);
        free(pristine);
        printf("sectors changed by the host: %u, refused: %u\n", changed, refused);

        usbs_end(&r);
        printf("outcome=%d status=%d time_set=%d device_set=%d session=%d bad=%d\n",
               (int)r.outcome, (int)r.rep.status, (int)r.time_set, (int)r.device_set,
               (int)r.session_start, (int)r.rep.bad_directive);
        if (r.session_start) {
            printf("module=%s\nlecture=%s\n", r.module, r.lecture);
        }
        devcfg_load(&cfg);
        printf("device id now %u\n", cfg.device_id);
        plat_rtc_get(&now);
        printf("clock %04u-%02u-%02u %02u:%02u:%02u\n", now.year, now.month, now.day, now.hour, now.minute, now.second);
        return 0;
    }
    return 2;
}
