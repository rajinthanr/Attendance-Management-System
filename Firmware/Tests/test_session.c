/**
 * @file    test_session.c
 * @brief   Host tests for lecture sessions: the marker format, the in-session
 *          duplicate check, how markers are hidden in ATTEND.CSV, and starting
 *          a session over USB.
 */
#include <stdlib.h>
#include "test_util.h"
#include "test_hostfs.h"

#include "csv.h"
#include "device_cfg.h"
#include "session.h"
#include "log_store.h"
#include "record_buffer.h"
#include "settings_file.h"
#include "timeutil.h"
#include "usb_storage.h"

static log_store_t     g_ls;
static record_buffer_t g_rb;
static hostfs_t        g_hf;

static const app_datetime_t k_now = { 2026u, 9u, 10u, 13u, 27u, 45u };

static void flush_all(void)
{
    (void)log_flush(&g_ls, &g_rb);
}

static void add(uint32_t id, uint32_t stamp)
{
    app_record_t r;

    r.student_id = id;
    r.stamp = stamp;
    if (rb_count(&g_rb) >= (APP_RAM_RECORDS - SESS_MAX_RECORDS)) {
        flush_all();
    }
    (void)rb_push(&g_rb, &r);
}

static void mark(const char *module, const char *lecture, uint32_t stamp)
{
    app_record_t recs[SESS_MAX_RECORDS];
    uint16_t n = sess_encode(recs, SESS_MAX_RECORDS, stamp, module, lecture);
    uint16_t i;

    if (rb_count(&g_rb) >= (APP_RAM_RECORDS - SESS_MAX_RECORDS)) {
        flush_all();
    }
    for (i = 0u; i < n; i++) {
        (void)rb_push(&g_rb, &recs[i]);
    }
}

static void fresh(void)
{
    host_flash_erase_all();
    log_init(&g_ls);
    rb_init(&g_rb);
    host_set_time(&k_now);
}

/* ===================================================================== */
/* The marker format                                                      */
/* ===================================================================== */

static void test_marker_format(void)
{
    app_record_t recs[SESS_MAX_RECORDS];
    session_t s;
    uint16_t n;

    printf("session marker format\n");

    n = sess_encode(recs, SESS_MAX_RECORDS, 12345u, "EN2090", "Circuits Lecture 4");
    CHECK(n > 1u && n <= SESS_MAX_RECORDS, "record count %u", n);
    CHECK(sess_is_header(recs[0].student_id) && sess_is_marker(recs[0].student_id), "header id");
    CHECK(sess_span(recs[0].student_id) == n, "span matches the count");
    CHECK(recs[0].stamp == 12345u, "start stamp");
    CHECK(recs[1].student_id == NV_MARK_TEXT && sess_is_marker(recs[1].student_id) &&
          !sess_is_header(recs[1].student_id), "text records are markers but not headers");
    CHECK(sess_decode(recs, n, &s) && s.valid, "decode");
    CHECK(strcmp(s.module, "EN2090") == 0 && strcmp(s.lecture, "Circuits Lecture 4") == 0, "names [%s|%s]", s.module, s.lecture);
    CHECK(s.start == 12345u, "start");

    /* "EN2090\0Circuits Lecture 4\0" is 6+1+18+1 = 26 bytes: 7 text records. */
    CHECK(n == 1u + 7u, "26 bytes need 7 text records, got %u", n - 1u);

    n = sess_encode(recs, SESS_MAX_RECORDS, 1u, "", "");
    CHECK(n == 2u && sess_decode(recs, n, &s) && s.module[0] == '\0' && s.lecture[0] == '\0', "both empty");
    n = sess_encode(recs, SESS_MAX_RECORDS, 1u, NULL, NULL);
    CHECK(n == 2u && sess_decode(recs, n, &s) && s.valid && s.module[0] == '\0', "NULL names are empty");
    n = sess_encode(recs, SESS_MAX_RECORDS, 1u, "M", "");
    CHECK(sess_decode(recs, n, &s) && strcmp(s.module, "M") == 0 && s.lecture[0] == '\0', "module only");
    n = sess_encode(recs, SESS_MAX_RECORDS, 1u, "", "L");
    CHECK(sess_decode(recs, n, &s) && s.module[0] == '\0' && strcmp(s.lecture, "L") == 0, "lecture only");

    /* Longest names: 24 + 1 + 32 + 1 = 58 bytes = 15 text records, the most a header can count. */
    n = sess_encode(recs, SESS_MAX_RECORDS, 7u, "ABCDEFGHIJKLMNOPQRSTUVWX",
                    "abcdefghijklmnopqrstuvwxyz012345");
    CHECK(n == 16u, "longest marker is 16 records (%u)", n);
    CHECK(sess_decode(recs, n, &s) && strcmp(s.module, "ABCDEFGHIJKLMNOPQRSTUVWX") == 0 &&
          strcmp(s.lecture, "abcdefghijklmnopqrstuvwxyz012345") == 0, "longest names survive");

    /* Over the limits: cut, never overflowing, never splitting a character. */
    n = sess_encode(recs, SESS_MAX_RECORDS, 7u, "ABCDEFGHIJKLMNOPQRSTUVWXYZ-OVERFLOW",
                    "abcdefghijklmnopqrstuvwxyz0123456789-OVERFLOW");
    CHECK(n == 16u && sess_decode(recs, n, &s), "oversize names still encode");
    CHECK(strcmp(s.module, "ABCDEFGHIJKLMNOPQRSTUVWX") == 0 && strcmp(s.lecture, "abcdefghijklmnopqrstuvwxyz012345") == 0,
          "cut at 24 and 32 [%s|%s]", s.module, s.lecture);
    n = sess_encode(recs, SESS_MAX_RECORDS, 7u, "abcdefghijklmnopqrstuvw" "\xC3\xA9" "z", "x");
    CHECK(sess_decode(recs, n, &s) && strcmp(s.module, "abcdefghijklmnopqrstuvw") == 0,
          "a UTF-8 character split by the cut is dropped whole [%s]", s.module);
    n = sess_encode(recs, SESS_MAX_RECORDS, 7u, "\xE0\xB7\x83\xE0\xB7\x92\xE0\xB6\xB1", "\xE0\xB7\x8A");
    CHECK(sess_decode(recs, n, &s) && strcmp(s.module, "\xE0\xB7\x83\xE0\xB7\x92\xE0\xB6\xB1") == 0 &&
          strcmp(s.lecture, "\xE0\xB7\x8A") == 0, "multi-byte names round trip");

    CHECK(sess_encode(recs, 3u, 1u, "EN2090", "Lecture") == 0u, "too small a buffer is refused");
    CHECK(sess_encode(recs, 1u, 1u, "", "") == 0u, "even an empty marker needs two records");

    /* Decoding refuses anything that is not a whole, well formed marker. */
    n = sess_encode(recs, SESS_MAX_RECORDS, 1u, "EN2090", "L");
    CHECK(!sess_decode(recs, (uint16_t)(n - 1u), &s) && !s.valid, "truncated marker");
    CHECK(!sess_decode(recs, 0u, &s), "no records");
    recs[1].student_id = 1000u;
    CHECK(!sess_decode(recs, n, &s), "a text record that is a card");
    recs[0].student_id = 1000u;
    CHECK(!sess_decode(recs, n, &s), "a header that is a card");

    CHECK(!sess_is_marker(1000u) && !sess_is_marker(NV_ID_RESERVED_MIN - 1u) &&
          sess_is_marker(NV_ID_RESERVED_MIN) && sess_is_marker(0xFFFFFFFFu), "marker id range");
    CHECK(sess_is_header(0xFFFFFFF0u) && sess_is_header(0xFFFFFFFFu) && !sess_is_header(0xFFFFFFEFu) &&
          !sess_is_header(NV_MARK_TEXT), "header id range");

    /* Through the log. */
    fresh();
    add(1000u, 10u);
    mark("EN2090", "Lecture 1", 20u);
    add(1007u, 30u);
    flush_all();
    CHECK(log_total(&g_ls) == 1u + (1u + 5u) + 1u, "marker records are ordinary log records (%u)", log_total(&g_ls));
    CHECK(sess_read(&g_ls, 1u, &s) && s.start == 20u && strcmp(s.lecture, "Lecture 1") == 0, "read back from flash");
    CHECK(!sess_read(&g_ls, 0u, &s) && !s.valid, "index 0 is a card, not a marker");
    CHECK(!sess_read(&g_ls, 3u, &s), "a text record is not a header");
    CHECK(!sess_read(&g_ls, 99u, &s), "past the end");
}

/* ===================================================================== */
/* The duplicate check                                                    */
/* ===================================================================== */

static void test_card_seen(void)
{
    const uint32_t day = 86400u;
    const uint32_t t0 = 5u * day;
    const uint32_t age = APP_SESSION_MAX_AGE_S;

    printf("session duplicate check\n");

    fresh();
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "nothing logged yet");
    CHECK(!sess_card_seen(NULL, NULL, 1000u, t0, age, 100u), "no log, no buffer");

    add(1000u, t0 - 100u);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "found in the RAM buffer");
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1007u, t0, age, 100u), "another card is not");
    CHECK(!sess_card_seen(&g_ls, NULL, 1000u, t0, age, 100u), "the RAM buffer is what held it");

    flush_all();
    rb_init(&g_rb);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "found in flash after the buffer is flushed");
    log_init(&g_ls);
    CHECK(sess_card_seen(&g_ls, NULL, 1000u, t0, age, 100u), "and after a reboot rebuilt the index");

    /* Age limit. */
    CHECK(sess_card_seen(&g_ls, NULL, 1000u, t0 - 100u + age, age, 100u), "exactly at the limit still counts");
    CHECK(!sess_card_seen(&g_ls, NULL, 1000u, t0 - 100u + age + 1u, age, 100u), "one second past it does not");

    /* The age limit applies to records still in the RAM buffer too. */
    fresh();
    add(1000u, t0 - age - 10u);
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "an old record in RAM does not count");
    add(1007u, t0 - 5u);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1007u, t0, age, 100u), "a recent one does");
    fresh();
    add(1000u, t0 - age + 10u);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "just inside the limit counts, in RAM");
    fresh();
    add(1000u, t0 - 100u);
    flush_all();

    /* A clock set backwards makes stamps look like the future: treated as new. */
    CHECK(sess_card_seen(&g_ls, NULL, 1000u, t0 - 5u * 3600u, age, 100u), "a stamp in the future counts as just now");

    /* A session header ends the look-back. */
    fresh();
    add(1000u, t0 - 500u);
    mark("EN2090", "Lecture 2", t0 - 400u);
    flush_all();
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "taps before the lecture started do not count");
    add(1000u, t0 - 300u);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "taps after it do");
    flush_all();
    rb_init(&g_rb);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "...from flash too");

    /* A marker still in the RAM buffer ends it as well. */
    fresh();
    add(1000u, t0 - 500u);
    flush_all();
    mark("M", "L", t0 - 400u);
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "a marker waiting in RAM ends the look-back");

    /* Text records never look like a card, whatever their bytes say. */
    fresh();
    mark("1000", "ABCD", t0 - 100u);
    CHECK(!sess_card_seen(&g_ls, &g_rb, 0x30303031u, t0, age, 100u), "marker text is not a card id");
    CHECK(!sess_card_seen(&g_ls, &g_rb, NV_MARK_TEXT, t0, age, 100u), "not even the text tag");

    /* The scan is bounded. */
    fresh();
    add(1000u, t0 - 1000u);
    {
        uint32_t i;

        for (i = 0u; i < 300u; i++) {
            add(2000u + i, t0 - 900u + i);
        }
    }
    flush_all();
    rb_init(&g_rb);
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 400u), "reached within the scan limit");
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 100u), "beyond the scan limit it is not looked for");
    CHECK(sess_card_seen(&g_ls, &g_rb, 2299u, t0, age, 1u), "the newest record is always examined");

    /* The limit is shared by the buffer and the log. */
    fresh();
    add(1000u, t0 - 100u);
    flush_all();
    add(2000u, t0 - 50u);
    add(2001u, t0 - 40u);
    CHECK(!sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 2u), "two records examined: the buffer's, not flash's");
    CHECK(sess_card_seen(&g_ls, &g_rb, 1000u, t0, age, 3u), "three reach the flash record");
}

/* ===================================================================== */
/* Rows and markers in ATTEND.CSV                                         */
/* ===================================================================== */

static char *attend_text(uint32_t *len)
{
    int32_t n;
    char *buf;

    usbs_begin(&g_ls, &(device_cfg_t){ true, 0xC0FFEEu, 0u, 0u }, &k_now);
    hf_mount(&g_hf);
    buf = (char *)malloc(1024u * 1024u);
    n = hf_read(&g_hf, HF_ATTEND, (uint8_t *)buf, 1024u * 1024u - 1u);
    if (n < 0) { n = 0; }
    buf[n] = '\0';
    *len = (uint32_t)n;
    return buf;
}

static bool row_is(const char *text, uint32_t r, uint32_t id)
{
    const char *row = &text[r * CSV_ROW_BYTES];
    char idtxt[16];

    snprintf(idtxt, sizeof(idtxt), "%010u", id);
    return memcmp(&row[20], idtxt, 10u) == 0 && row[30] == '\r' && row[31] == '\n';
}

static void test_export_with_markers(void)
{
    uint32_t len;
    char *t;

    printf("attendance export with session markers\n");

    /* d0 d1 [A] d2 d3 d4 [B][C] d5 [D] */
    fresh();
    add(1000u, 100u);                 /* row 1 */
    add(1007u, 101u);                 /* row 2 */
    mark("EN2090", "Lecture A", 102u);
    add(1014u, 103u);                 /* row 3 */
    add(1021u, 104u);                 /* row 4 */
    add(1028u, 105u);                 /* row 5 */
    mark("EN2090", "Lecture B", 106u);
    mark("MA1010", "Lecture C", 107u);
    add(1035u, 108u);                 /* row 6 */
    mark("PH1000", "Lecture D", 109u);
    flush_all();

    t = attend_text(&len);
    CHECK(len == csv_size(6u), "6 attendance rows plus the header, markers excluded (%u)", len);
    CHECK(usbs_file_size() == csv_size(6u), "reported size agrees");
    CHECK(row_is(t, 1u, 1000u) && row_is(t, 2u, 1007u) && row_is(t, 3u, 1014u) &&
          row_is(t, 4u, 1021u) && row_is(t, 5u, 1028u) && row_is(t, 6u, 1035u), "every row maps to the right record");
    CHECK(strstr(t, "4294967") == NULL, "no marker id leaks out");
    CHECK(strstr(t, "Lecture") == NULL && strstr(t, "EN2090") == NULL, "lecture names are not in the CSV: the PC knows the lectures");

    /* The lecture on show is the newest, even though it has no rows yet. */
    {
        static char file[4096];
        int32_t n = hf_read(&g_hf, HF_SETTINGS, (uint8_t *)file, sizeof(file) - 1u);

        file[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(file, "#MODULE,PH1000\r\n#LECTURE,Lecture D\r\n") != NULL, "SETTINGS.CSV shows the newest session");
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)file, 512u);
        file[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(file, "Lecture      : PH1000 / Lecture D") != NULL, "STATUS.TXT shows it too");
        CHECK(strstr(file, "Lecture D (since 2000-01-01 00:01:49)") != NULL, "with the device-clock time it started [%s]", file);
        CHECK(strstr(file, "Attendance   : 6 records") != NULL, "and counts attendance, not markers");
        CHECK(strstr(file, "Last tap     : 0000001035 at 2000-01-01 00:01:48") != NULL, "the last tap is the last attendance record, not a marker");
    }
    free(t);

    /* Reading rows in any order gives the same rows (random access). */
    {
        uint8_t sec[512];
        uint32_t att = USBS_ATTEND_CLUSTER;
        uint32_t s;
        uint32_t bad = 0u;
        char *ref;

        ref = attend_text(&len);
        for (s = (len + 511u) / 512u; s > 0u; s--) {
            uint32_t want = len - ((s - 1u) * 512u);

            (void)usbs_read(fat12_cluster_lba(att) + (s - 1u), sec, 1u);
            if (memcmp(sec, &ref[(s - 1u) * 512u], (want > 512u) ? 512u : want) != 0) {
                bad++;
            }
        }
        CHECK(bad == 0u, "%u sectors differ when read newest first", bad);
        free(ref);
    }

    /* A marker first in the log, and a log that is only markers. */
    fresh();
    mark("EN2090", "First", 50u);
    add(1000u, 51u);
    flush_all();
    t = attend_text(&len);
    CHECK(len == csv_size(1u) && row_is(t, 1u, 1000u), "marker at the very start");
    free(t);

    fresh();
    mark("EN2090", "Only", 50u);
    mark("EN2090", "Two", 60u);
    flush_all();
    t = attend_text(&len);
    CHECK(len == csv_size(0u), "only markers: a header and nothing else");
    CHECK(usbs_sector_count() == FAT12_DATA_START_LBA + 1u + USBS_SETTINGS_CLUSTERS + 1u + 1u,
          "one ATTEND cluster, one LECTURES cluster");
    free(t);

    /* An orphaned text record (a marker whose header was lost) is skipped, not shown. */
    fresh();
    add(1000u, 10u);
    {
        app_record_t stray;

        stray.student_id = NV_MARK_TEXT;
        stray.stamp = 0x41424344u;
        (void)rb_push(&g_rb, &stray);
    }
    add(1007u, 11u);
    flush_all();
    t = attend_text(&len);
    CHECK(len == csv_size(2u) && row_is(t, 1u, 1000u) && row_is(t, 2u, 1007u), "a stray text record is not a row");
    free(t);

    /* A marker torn by a power cut: the header promises five text records and
     * two arrive. What follows is attendance and must stay attendance. */
    fresh();
    add(1000u, 10u);
    {
        app_record_t recs[SESS_MAX_RECORDS];
        uint16_t n = sess_encode(recs, SESS_MAX_RECORDS, 20u, "EN2090", "Lecture 1");
        uint16_t i;

        CHECK(n == 6u, "a 6 record marker to tear");
        for (i = 0u; i < 3u; i++) {                      /* header + 2 of 5 text records */
            (void)rb_push(&g_rb, &recs[i]);
        }
    }
    add(1007u, 30u);
    add(1014u, 31u);
    flush_all();
    t = attend_text(&len);
    CHECK(len == csv_size(3u), "the three attendance records survive a torn marker (%u rows)", (len / CSV_ROW_BYTES) - 1u);
    CHECK(row_is(t, 1u, 1000u) && row_is(t, 2u, 1007u) && row_is(t, 3u, 1014u), "and map to the right rows");
    free(t);

    /* The tail of a torn marker with its header gone, after a good one. */
    fresh();
    mark("EN2090", "Lecture 1", 5u);
    add(1000u, 10u);
    {
        app_record_t stray = { NV_MARK_TEXT, 0x41424344u };

        (void)rb_push(&g_rb, &stray);
        (void)rb_push(&g_rb, &stray);
    }
    add(1007u, 30u);
    add(1014u, 31u);
    flush_all();
    t = attend_text(&len);
    CHECK(len == csv_size(3u), "stray text records are not rows (%u)", (len / CSV_ROW_BYTES) - 1u);
    CHECK(row_is(t, 1u, 1000u) && row_is(t, 2u, 1007u) && row_is(t, 3u, 1014u), "rows after strays map correctly");
    free(t);

    /* Many lectures and many rows: the mapping holds at scale. */
    fresh();
    {
        uint32_t lecture, k, expect_rows = 0u;
        char name[16];
        uint32_t bad = 0u;

        for (lecture = 0u; lecture < 60u; lecture++) {
            snprintf(name, sizeof(name), "Lecture %02u", lecture);
            mark("EN2090", name, 1000u + (lecture * 100u));
            for (k = 0u; k < 25u; k++) {
                add(1000u + (7u * k), 1000u + (lecture * 100u) + k + 1u);
                expect_rows++;
            }
        }
        flush_all();
        CHECK(log_total(&g_ls) == expect_rows + (60u * 6u), "log holds %u records", log_total(&g_ls));
        t = attend_text(&len);
        CHECK(len == csv_size(expect_rows), "1500 rows, markers hidden (%u)", len);
        for (lecture = 0u; lecture < 60u; lecture++) {
            for (k = 0u; k < 25u; k++) {
                if (!row_is(t, 1u + (lecture * 25u) + k, 1000u + (7u * k))) {
                    bad++;
                }
            }
        }
        CHECK(bad == 0u, "%u of 1500 rows carry the wrong card", bad);
        free(t);
    }

    /* A full-size log still fits the volume. */
    fresh();
    {
        uint32_t i;
        uint32_t total_rows = 0u;

        mark("EN2090", "Big", 10u);
        for (i = 0u; i < NV_LOG_CAPACITY - 8u; i++) {
            add(1000u + (7u * (i % 20u)), 100u + i);
            total_rows++;
            if ((i % 1000u) == 999u) { flush_all(); }
        }
        flush_all();
        usbs_begin(&g_ls, &(device_cfg_t){ true, 0xC0FFEEu, 0u, 0u }, &k_now);
        CHECK(usbs_file_size() == csv_size(total_rows), "near-full log: %u rows (%u)", total_rows, usbs_file_size());
        CHECK(fat12_cluster_count(usbs_sector_count()) < 2047u, "within the FAT (%u clusters)", fat12_cluster_count(usbs_sector_count()));
        CHECK(fat12_cluster_count(usbs_sector_count()) > 800u, "and it really is a large volume");
        {
            uint8_t sec[512];
            const uint32_t last = ((csv_size(total_rows) + 511u) / 512u) - 1u;

            CHECK(usbs_read(fat12_cluster_lba(USBS_ATTEND_CLUSTER) + last, sec, 1u), "last sector reads");
            CHECK(sec[0] == '2', "and holds a row");
        }
    }
}

/* ===================================================================== */
/* Starting a lecture over USB                                            */
/* ===================================================================== */

static usbs_result_t import_text(const char *directives)
{
    usbs_result_t r;

    (void)hf_create(&g_hf, HF_SETTINGS, directives, (uint32_t)strlen(directives));
    usbs_end(&r);
    return r;
}

static void begin(void)
{
    usbs_begin(&g_ls, &(device_cfg_t){ true, 0xC0FFEEu, 0u, 0u }, &k_now);
    hf_mount(&g_hf);
}

static void test_session_start(void)
{
    usbs_result_t r;
    static char status[513];

    printf("starting a lecture from SETTINGS.CSV\n");

    /* No lecture yet: naming one starts it. */
    fresh();
    begin();
    r = import_text("#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n");
    CHECK(r.outcome == USBS_IMPORT_OK && r.session_start, "naming a lecture starts one");
    CHECK(strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Lecture 1") == 0, "names [%s|%s]", r.module, r.lecture);

    /* Neither directive, or the blank slots as shown: nothing starts. */
    fresh();
    begin();
    r = import_text("# just a comment\r\n");
    CHECK(r.outcome == USBS_IMPORT_OK && !r.session_start, "a file with no directives starts nothing");
    fresh();
    begin();
    r = import_text("#MODULE,\r\n#LECTURE,\r\n");
    CHECK(r.outcome == USBS_IMPORT_OK && !r.session_start, "the blank slots as shown start nothing");

    /* With a lecture running: editing, repeating, omitting. */
    fresh();
    mark("EN2090", "Lecture 1", 10u);
    add(1000u, 11u);
    flush_all();

    begin();
    r = import_text("#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n");
    CHECK(r.outcome == USBS_IMPORT_OK && !r.session_start, "the same names: the lecture carries on");

    begin();
    r = import_text("#MODULE,EN2090\r\n#LECTURE,Lecture 2\r\n");
    CHECK(r.session_start && strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Lecture 2") == 0, "a new lecture name starts one");

    begin();
    r = import_text("#MODULE,MA1010\r\n#LECTURE,Lecture 1\r\n");
    CHECK(r.session_start && strcmp(r.module, "MA1010") == 0, "a new module starts one");

    begin();
    r = import_text("#LECTURE,Lecture 9\r\n");
    CHECK(r.session_start && strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Lecture 9") == 0,
          "omitting #MODULE keeps the current one");

    begin();
    r = import_text("#MODULE,PH1000\r\n");
    CHECK(r.session_start && strcmp(r.module, "PH1000") == 0 && strcmp(r.lecture, "Lecture 1") == 0,
          "omitting #LECTURE keeps the current one");

    begin();
    r = import_text("#NEWSESSION,1\r\n");
    CHECK(r.session_start && strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Lecture 1") == 0,
          "#NEWSESSION alone repeats the current names");

    begin();
    r = import_text("#MODULE,\r\n#LECTURE,\r\n");
    CHECK(r.session_start && r.module[0] == '\0' && r.lecture[0] == '\0', "clearing the names ends the lecture");

    begin();
    r = import_text("#module,en2090\r\n");
    CHECK(r.session_start, "names are case sensitive (en2090 is not EN2090)");

    begin();
    r = import_text("#MODULE,\"EN2090\"\r\n#LECTURE,\"Lecture 1\"\r\n");
    CHECK(!r.session_start, "quotes around the same names change nothing");

    begin();
    r = import_text("#LECTURE,\"Intro, part 2\"\r\n");
    CHECK(r.session_start && strcmp(r.lecture, "Intro  part 2") == 0, "a comma in a name becomes a space [%s]", r.lecture);

    begin();
    r = import_text("#MODULE,ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n#LECTURE,abcdefghijklmnopqrstuvwxyz0123456789\r\n");
    CHECK(r.session_start && strcmp(r.module, "ABCDEFGHIJKLMNOPQRSTUVWX") == 0 &&
          strcmp(r.lecture, "abcdefghijklmnopqrstuvwxyz012345") == 0, "long names are cut to 24 and 32 [%s|%s]", r.module, r.lecture);

    /* A refused file starts nothing, whatever it says. */
    begin();
    {
        static const char bad[] = "#MODULE,EN9999\r\n#LECTURE,Nope\r\n";

        (void)hf_create(&g_hf, HF_SETTINGS, bad, 0u);
        usbs_end(&r);
        CHECK(r.outcome == USBS_IMPORT_FAILED && !r.session_start && r.module[0] == '\0',
              "a refused file starts no lecture");
    }

    /* STATUS.TXT announces it before the cable is pulled. */
    begin();
    {
        static const char t[] = "#MODULE,EN2090\r\n#LECTURE,Lecture 5\r\n";
        int32_t n;

        (void)hf_create(&g_hf, HF_SETTINGS, t, (uint32_t)strlen(t));
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)status, 512u);
        status[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(status, "a new lecture will start") != NULL, "status announces a new lecture [%s]", status);
        CHECK(strstr(status, "Lecture      : EN2090 / Lecture 1") != NULL, "status still shows the one in progress");
    }
    {
        static const char t[] = "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n";
        int32_t n;

        (void)hf_create(&g_hf, HF_SETTINGS, t, (uint32_t)strlen(t));
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)status, 512u);
        status[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(status, "a new lecture will start") == NULL, "no announcement when the names are unchanged");
    }
    {
        usbs_result_t q;

        usbs_end(&q);
    }
}

/* ===================================================================== */
/* Lectures started on the device: the next name, the newest marker       */
/* ===================================================================== */

static bool next_is(const char *cur, const char *want)
{
    char out[SESS_LECTURE_MAX + 1u];

    memset(out, 'x', sizeof(out));
    sess_next_name(cur, out);
    if (strcmp(out, want) != 0) {
        printf("    next(\"%s\") = \"%s\", want \"%s\"\n", cur != NULL ? cur : "(null)", out, want);
        return false;
    }
    return true;
}

static void test_lecture_names(void)
{
    session_t s;

    printf("lectures started on the device\n");

    CHECK(next_is("Circuits Lecture 4", "Circuits Lecture 5"), "a trailing number counts up");
    CHECK(next_is("Lecture 9", "Lecture 10"), "with a carry");
    CHECK(next_is("Week 09", "Week 10"), "keeping leading zeros");
    CHECK(next_is("L99", "L100"), "growing a digit");
    CHECK(next_is("999", "1000"), "a name that is only a number");
    CHECK(next_is("Tutorial", "Tutorial 2"), "no number: \" 2\" is added");
    CHECK(next_is("Lab 2b", "Lab 2b 2"), "a number not at the end does not count");
    CHECK(next_is("", "Lecture 1"), "no previous lecture");
    CHECK(next_is(NULL, "Lecture 1"), "NULL is no previous lecture");
    /* 32 bytes, the most a lecture name holds: the text before the number is cut. */
    CHECK(next_is("Introduction to circuit theor 99", "Introduction to circuit theo 100"), "cut to fit");
    CHECK(next_is("Introduction to circuit theory a", "Introduction to circuit theory 2"), "cut to fit, no number");
    /* ...never through a UTF-8 character: "é" is two bytes. */
    CHECK(next_is("Leçon numéro un à la finé 99", "Leçon numéro un à la fin 100"), "cut at a character boundary");
    CHECK(next_is("Leçon numéro un à la fiéé99", "Leçon numéro un à la fié100"), "cut at a character boundary, 2");
    CHECK(next_is("99999999999999999999999999999999", "10000000000000000000000000000000"), "32 nines");

    fresh();
    CHECK(!sess_latest(&g_ls, &s) && !s.valid, "an empty log has no lecture");
    add(1000u, 10u);
    flush_all();
    CHECK(!sess_latest(&g_ls, &s), "taps alone are no lecture");
    mark("EN2090", "Circuits Lecture 4", 50u);
    add(1001u, 60u);
    mark("EN2090", "Circuits Lecture 5", 100u);
    add(1002u, 110u);
    add(1003u, 120u);
    flush_all();
    CHECK(sess_latest(&g_ls, &s) && s.valid && s.start == 100u && strcmp(s.module, "EN2090") == 0 &&
          strcmp(s.lecture, "Circuits Lecture 5") == 0, "the newest marker, past the taps after it");
}

/** LECTURES.CSV through the volume, as a host would read it. */
static char *lectures_text(uint32_t *len)
{
    int32_t n;
    char *buf;

    usbs_begin(&g_ls, &(device_cfg_t){ true, 0xC0FFEEu, 0u, 0u }, &k_now);
    hf_mount(&g_hf);
    buf = (char *)malloc(256u * 1024u);
    n = hf_read(&g_hf, HF_LECTURES, (uint8_t *)buf, 256u * 1024u - 1u);
    if (n < 0) { n = 0; }
    buf[n] = '\0';
    *len = (uint32_t)n;
    return buf;
}

/** Row @p r of LECTURES.CSV is @p text, space padded, then CRLF. */
static bool lecture_row_is(const char *csv, uint32_t r, const char *text)
{
    const char *row = &csv[r * CSV_LECTURE_ROW_BYTES];
    size_t n = strlen(text);
    size_t i;

    if (memcmp(row, text, n) != 0) {
        printf("    row %u: \"%.40s\", want \"%s\"\n", r, row, text);
        return false;
    }
    for (i = n; i < CSV_LECTURE_ROW_BYTES - 2u; i++) {
        if (row[i] != ' ') {
            return false;
        }
    }
    return row[CSV_LECTURE_ROW_BYTES - 2u] == '\r' && row[CSV_LECTURE_ROW_BYTES - 1u] == '\n';
}

static void test_lectures_csv(void)
{
    static const char k_mod24[] = "ABCDEFGHIJKLMNOPQRSTUVWX";
    static const char k_lec32[] = "abcdefghijklmnopqrstuvwxyz012345";
    char *t;
    uint32_t len, i;
    char want[128];

    printf("LECTURES.CSV\n");

    fresh();
    t = lectures_text(&len);
    CHECK(len == CSV_LECTURE_ROW_BYTES && lecture_row_is(t, 0u, "DATE,TIME,MODULE,LECTURE"), "no lectures: the header alone");
    free(t);

    fresh();
    add(1000u, 10u);
    mark("EN2090", "Circuits Lecture 4", 50u);
    add(1001u, 60u);
    mark("", "Lecture 1", 3661u);
    add(1002u, 3700u);
    {
        app_record_t stray;     /* the tail of a torn marker: not a lecture */

        stray.student_id = NV_MARK_TEXT;
        stray.stamp = 0x41424344u;
        (void)rb_push(&g_rb, &stray);
    }
    mark(k_mod24, k_lec32, 86400u);
    flush_all();
    t = lectures_text(&len);
    CHECK(len == csv_lecture_size(3u), "three lectures (%u bytes)", len);
    CHECK(lecture_row_is(t, 1u, "2000-01-01,00:00:50,EN2090,Circuits Lecture 4"), "a lecture row");
    CHECK(lecture_row_is(t, 2u, "2000-01-01,01:01:01,,Lecture 1"), "an empty module stays an empty field");
    snprintf(want, sizeof(want), "2000-01-02,00:00:00,%s,%s", k_mod24, k_lec32);
    CHECK(lecture_row_is(t, 3u, want), "the longest names fit");
    free(t);
    t = attend_text(&len);
    CHECK(len == csv_size(3u), "ATTEND.CSV is unchanged: three taps");
    free(t);

    /* Enough lectures to span sectors: row 4 starts the second one. */
    fresh();
    for (i = 0u; i < 9u; i++) {
        snprintf(want, sizeof(want), "Lecture %u", i + 1u);
        mark("EN2090", want, 100u * (i + 1u));
        add(2000u + i, (100u * (i + 1u)) + 10u);
    }
    flush_all();
    t = lectures_text(&len);
    CHECK(len == csv_lecture_size(9u), "nine lectures");
    for (i = 0u; i < 9u; i++) {
        char text[96];
        app_datetime_t dt;

        time_from_epoch(100u * (i + 1u), &dt);
        snprintf(text, sizeof(text), "%04u-%02u-%02u,%02u:%02u:%02u,EN2090,Lecture %u",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, i + 1u);
        CHECK(lecture_row_is(t, i + 1u, text), "row %u across sectors", i + 1u);
    }
    free(t);
    {
        usbs_result_t q;

        usbs_end(&q);
    }
}

void test_sessions(void)
{
    test_marker_format();
    test_card_seen();
    test_export_with_markers();
    test_session_start();
    test_lecture_names();
    test_lectures_csv();
}
