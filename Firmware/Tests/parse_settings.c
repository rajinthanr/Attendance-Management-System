/**
 * @file    parse_settings.c
 * @brief   Run the firmware's own SETTINGS.CSV parser on files and print what
 *          it understood. Used by Companion/tests/test_firmware_compat.py to
 *          prove that what the app writes is what the device reads.
 *
 *   parse_settings file...
 *
 * One block per file:
 *   FILE <name>
 *   status=<n> time=<0|1> device=<n|-> new=<0|1> bad=<0|1>
 *   time_value=<YYYY-MM-DD HH:MM:SS>        (only when a #TIME was read)
 *   module=<text>
 *   lecture=<text>
 *   cards=<n|->                             (- when the file has no #CARDS list)
 *   cards_crc=<8 hex digits>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_file.h"

typedef struct { const uint8_t *p; uint32_t n; } mem_t;

static int mem_get(void *ctx, uint32_t off)
{
    const mem_t *m = (const mem_t *)ctx;

    return (off < m->n) ? m->p[off] : -1;
}

int main(int argc, char **argv)
{
    int a;

    for (a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        uint8_t *buf;
        long size;
        mem_t m;
        setf_report_t r;

        printf("FILE %s\n", argv[a]);
        if (f == NULL) {
            printf("status=-1\n");
            continue;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (uint8_t *)malloc((size_t)size + 1u);
        if (fread(buf, 1u, (size_t)size, f) != (size_t)size) { return 2; }
        fclose(f);

        m.p = buf;
        m.n = (uint32_t)size;
        setf_scan(mem_get, &m, m.n, &r);
        printf("status=%d time=%d device=", (int)r.status, (int)r.has_time);
        if (r.has_device) { printf("%u", r.device_id); } else { printf("-"); }
        printf(" new=%d bad=%d\n", (int)r.new_session, (int)r.bad_directive);
        if (r.has_time) {
            printf("time_value=%04u-%02u-%02u %02u:%02u:%02u\n", r.time.year, r.time.month, r.time.day,
                   r.time.hour, r.time.minute, r.time.second);
        }
        printf("module=%s\nlecture=%s\n", r.has_module ? r.module : "", r.has_lecture ? r.lecture : "");
        if (r.has_cards) { printf("cards=%u\n", r.card_count); } else { printf("cards=-\n"); }
        printf("cards_crc=%08x\n", r.card_crc);
        free(buf);
    }
    return 0;
}
