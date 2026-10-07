/**
 * @file    test_settings.c
 * @brief   Host tests for what the device exports and accepts over USB: the
 *          attendance CSV rows, the device-ID config page, the SETTINGS.CSV
 *          parser and renderer, the USB volume, and a simulated PC that edits
 *          SETTINGS.CSV in the ways real operating systems do.
 */
#include <stdlib.h>
#include "test_util.h"
#include "test_hostfs.h"

#include "csv.h"
#include "crc.h"
#include "fat12.h"
#include "device_cfg.h"
#include "log_store.h"
#include "record_buffer.h"
#include "settings_file.h"
#include "timeutil.h"
#include "usb_storage.h"
#include "platform_if.h"

/* ===================================================================== */
/* Shared fixtures                                                        */
/* ===================================================================== */

static log_store_t     g_ls;
static record_buffer_t g_rb;
static hostfs_t        g_hf;
static char            g_txt[16384];
static uint8_t         g_sec[FAT12_SECTOR_SIZE];

static const app_datetime_t k_now = { 2026u, 9u, 10u, 13u, 27u, 45u };

/**
 * Fresh flash with device ID @p device_id (0 = none) and @p records taps
 * logged, then a USB session started and mounted by the test host.
 *
 * Record i is card 1000 + 7*(i % 10), taken i seconds after k_now.
 */
static void begin_session(uint32_t records, uint32_t device_id)
{
    device_cfg_t cfg;
    uint32_t i;

    host_flash_erase_all();
    if (device_id != 0u) {
        (void)devcfg_set_device_id(&cfg, device_id);
    }
    devcfg_load(&cfg);

    log_init(&g_ls);
    rb_init(&g_rb);
    for (i = 0u; i < records; i++) {
        app_record_t rec;

        rec.student_id = 1000u + (7u * (i % 10u));
        rec.stamp = time_to_epoch(&k_now) + i;
        rb_push(&g_rb, &rec);
        if (rb_needs_flush(&g_rb)) { (void)log_flush(&g_ls, &g_rb); }
    }
    (void)log_flush(&g_ls, &g_rb);

    host_set_time(&k_now);
    usbs_begin(&g_ls, &cfg, &k_now);
    hf_mount(&g_hf);
}

static usbs_result_t end_session(void)
{
    usbs_result_t r;

    usbs_end(&r);
    return r;
}

/** Read the file the host was shown into g_txt. */
static uint32_t shown_file(void)
{
    int32_t n = hf_read(&g_hf, HF_SETTINGS, (uint8_t *)g_txt, sizeof(g_txt) - 1u);

    if (n < 0) { n = 0; }
    g_txt[n] = '\0';
    return (uint32_t)n;
}

/** Replace the first occurrence of @p from with @p to in g_txt (same or shorter). */
static void edit_text(const char *from, const char *to)
{
    char *p = strstr(g_txt, from);

    if (p != NULL) {
        size_t tail = strlen(p + strlen(from)) + 1u;

        memmove(p + strlen(to), p + strlen(from), tail);
        memcpy(p, to, strlen(to));
    }
}

/** The device clock as the RTC holds it. */
static app_datetime_t rtc_now(void)
{
    app_datetime_t dt;

    plat_rtc_get(&dt);
    return dt;
}

/** Copy @p text to the device as the host's SETTINGS.CSV (a replace). */
static bool put_settings(const char *text)
{
    return hf_create(&g_hf, HF_SETTINGS, text, (uint32_t)strlen(text));
}

/* ===================================================================== */
/* attendance CSV rows                                                    */
/* ===================================================================== */

void test_csv(void)
{
    char row[CSV_ROW_BYTES + 1];
    char expect[CSV_ROW_BYTES + 1];
    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 27u, 45u };
    app_record_t rec = { 123456u, time_to_epoch(&dt) };

    printf("csv\n");
    row[CSV_ROW_BYTES] = '\0';

    csv_header(row);
    snprintf(expect, sizeof(expect), "%-30s\r\n", "DATE,TIME,CARD_ID");
    CHECK(memcmp(row, expect, CSV_ROW_BYTES) == 0, "header [%s]", row);

    csv_row(&rec, row);
    CHECK(memcmp(row, "2026-09-10,13:27:45,0000123456\r\n", CSV_ROW_BYTES) == 0, "row [%s]", row);

    /* Extremes of the numeric columns keep their width. */
    rec.student_id = 0xFFFFFFFFu;
    rec.stamp = 0u;
    csv_row(&rec, row);
    CHECK(memcmp(row, "2000-01-01,00:00:00,4294967295\r\n", CSV_ROW_BYTES) == 0, "extremes [%s]", row);
    rec.student_id = 1u;
    csv_row(&rec, row);
    CHECK(memcmp(row, "2000-01-01,00:00:00,0000000001\r\n", CSV_ROW_BYTES) == 0, "leading zeros [%s]", row);

    /* The last second the epoch can name, and a leap day. */
    {
        app_datetime_t leap = { 2024u, 2u, 29u, 23u, 59u, 59u };

        rec.student_id = 7u;
        rec.stamp = time_to_epoch(&leap);
        csv_row(&rec, row);
        CHECK(memcmp(row, "2024-02-29,23:59:59,0000000007\r\n", CSV_ROW_BYTES) == 0, "leap day [%s]", row);
    }

    CHECK(CSV_ROW_BYTES == 32u, "row width");
    CHECK((512u % CSV_ROW_BYTES) == 0u, "rows divide a sector");
    CHECK(CSV_ROWS_PER_SECTOR == 16u, "rows per sector");
    CHECK(csv_size(0u) == CSV_ROW_BYTES, "empty file is just the header");
    CHECK(csv_size(100u) == 101u * 32u, "size of 100 records");
    CHECK(NV_LOG_CAPACITY * CSV_ROW_BYTES < 450000u, "a full log is under 450 kB");
}

/* ===================================================================== */
/* The device ID in the config page                                       */
/* ===================================================================== */

void test_device_cfg(void)
{
    device_cfg_t c;

    printf("device_cfg\n");

    host_flash_erase_all();
    devcfg_load(&c);
    CHECK(!c.valid && c.device_id == 0u, "blank flash has no config");

    CHECK(devcfg_set_device_id(&c, 12345u), "set");
    CHECK(c.valid && c.device_id == 12345u, "reads back");
    devcfg_load(&c);
    CHECK(c.valid && c.device_id == 12345u, "and after a reboot");

    CHECK(devcfg_set_device_id(&c, 0xC0FFEEu) && c.device_id == 0xC0FFEEu, "replaced");
    CHECK(devcfg_set_device_id(&c, 0u) && c.valid && c.device_id == 0u, "an ID of zero is a valid, empty setting");
    CHECK(devcfg_set_device_id(&c, 0xFFFFFFFEu) && c.device_id == 0xFFFFFFFEu, "the largest ID");

    /* A page written by earlier firmware (a student list descriptor) is not read. */
    {
        nv_config_t old;

        memset(&old, 0, sizeof(old));
        old.magic = NV_CONFIG_MAGIC;
        old.format_version = 2u;
        old.device_id = 99u;
        host_flash_erase_all();
        memcpy(&host_flash[NV_CONFIG_OFFSET], &old, sizeof(old));
        devcfg_load(&c);
        CHECK(!c.valid && c.device_id == 0u, "an old-format page is ignored");
        CHECK(devcfg_set_device_id(&c, 7u) && c.valid && c.device_id == 7u, "and can be replaced");
        old.magic = 0u;
        host_flash_erase_all();
        memcpy(&host_flash[NV_CONFIG_OFFSET], &old, sizeof(old));
        devcfg_load(&c);
        CHECK(!c.valid, "a page without the magic is ignored");
    }

    /* Power lost part-way: the page is erased first, so what remains is
     * "no ID", never a half-written one that looks valid. */
    host_flash_erase_all();
    CHECK(devcfg_set_device_id(&c, 5u), "an ID to lose");
    host_fail_writes_after(1u);
    CHECK(!devcfg_set_device_id(&c, 6u), "an interrupted write reports failure");
    host_write_failures = 0u;
    devcfg_load(&c);
    CHECK(!c.valid && c.device_id == 0u, "and leaves no config, not a wrong one");

    /* A write that "succeeds" but stores the wrong bit is caught by the read-back. */
    host_flash_erase_all();
    host_corrupt_write_after(0u);
    CHECK(!devcfg_set_device_id(&c, 0x1234u), "a silently wrong write is detected");
    host_corrupt_write = 0u;
}

/* ===================================================================== */
/* SETTINGS.CSV text: parser and renderer                                 */
/* ===================================================================== */

typedef struct { const uint8_t *p; uint32_t n; } mem_t;

static int mem_get(void *ctx, uint32_t off)
{
    const mem_t *m = (const mem_t *)ctx;

    return (off < m->n) ? m->p[off] : -1;
}

static setf_report_t scan_n(const void *text, uint32_t n)
{
    setf_report_t r;
    mem_t m;

    m.p = (const uint8_t *)text;
    m.n = n;
    setf_scan(mem_get, &m, n, &r);
    return r;
}

static setf_report_t scan_s(const char *text)
{
    return scan_n(text, (uint32_t)strlen(text));
}

void test_settings_parser(void)
{
    setf_report_t r;
    uint32_t n;

    printf("settings parser\n");

    /* ---- every directive ---- */
    r = scan_s("#TIME,2030-05-06 07:08:09\r\n#MODULE,EN2090\r\n#LECTURE,Circuits Lecture 4\r\n#DEVICE,0000012345\r\n#NEWSESSION,1\r\n");
    CHECK(r.status == SETF_OK && r.has_time && r.has_module && r.has_lecture && r.has_device && r.new_session && !r.bad_directive,
          "all five present");
    CHECK(r.time.year == 2030u && r.time.month == 5u && r.time.day == 6u && r.time.hour == 7u &&
          r.time.minute == 8u && r.time.second == 9u, "#TIME fields");
    CHECK(strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Circuits Lecture 4") == 0, "names [%s|%s]", r.module, r.lecture);
    CHECK(r.device_id == 12345u, "#DEVICE %u", r.device_id);

    r = scan_s("# nothing at all\r\n");
    CHECK(r.status == SETF_OK && !r.has_time && !r.has_module && !r.has_lecture && !r.has_device && !r.new_session,
          "a file of comments asks for nothing");

    /* ---- line ends, BOM, last line ---- */
    r = scan_s("#MODULE,A\n#LECTURE,B\n");
    CHECK(r.has_module && r.has_lecture, "LF only");
    r = scan_s("#MODULE,A\r#LECTURE,B\r");
    CHECK(r.has_module && r.has_lecture && strcmp(r.lecture, "B") == 0, "lone CR");
    r = scan_s("#MODULE,A\r\n#LECTURE,B");
    CHECK(strcmp(r.lecture, "B") == 0, "no final newline");
    r = scan_n("\xEF\xBB\xBF" "#MODULE,Bom\r\n", 3u + 13u);
    CHECK(r.has_module && strcmp(r.module, "Bom") == 0, "UTF-8 byte-order mark skipped");

    /* ---- time formats ---- */
    r = scan_s("  #time,2030-05-06T07:08\n");
    CHECK(r.has_time && r.time.second == 0u && r.time.minute == 8u, "lowercase, indented, T, no seconds");
    r = scan_s("#TIME,2030/5/6 7:08\n");
    CHECK(r.has_time && r.time.month == 5u && r.time.hour == 7u, "slashes and single digits");
    r = scan_s("#TIME,\"2030-05-06 07:08:09\"\n");
    CHECK(r.has_time, "quoted");
    r = scan_s("#TIME,2030-13-06 07:08:09\n");
    CHECK(!r.has_time && r.bad_directive && r.status == SETF_OK, "month 13 ignored, not fatal");
    r = scan_s("#TIME,2030-02-30 07:08:09\n");
    CHECK(!r.has_time && r.bad_directive, "30 February ignored");
    r = scan_s("#TIME,2028-02-29 23:59:59\n");
    CHECK(r.has_time, "29 February in a leap year");
    r = scan_s("#TIME,5/6/2030 7:08\n");
    CHECK(!r.has_time && r.bad_directive, "a locale date from a spreadsheet is ignored");
    r = scan_s("#TIME,\n");
    CHECK(!r.has_time && r.bad_directive, "empty #TIME ignored");
    r = scan_s("#TIME,1999-12-31 23:59:59\n");
    CHECK(!r.has_time && r.bad_directive, "before the epoch ignored");
    r = scan_s("#TIME,2100-01-01 00:00:00\n");
    CHECK(!r.has_time && r.bad_directive, "after 2099 ignored");
    r = scan_s("#TIME,2030-05-06 07:08:09:10\n");
    CHECK(!r.has_time && r.bad_directive, "too many fields ignored");
    r = scan_s("#TIME,2030-05-06 24:00:00\n");
    CHECK(!r.has_time && r.bad_directive, "hour 24 ignored");
    r = scan_s("#TIME,2030-05-06 07:08:99999\n");
    CHECK(!r.has_time && r.bad_directive, "a field that would wrap into range is refused");

    /* ---- device id ---- */
    r = scan_s("#DEVICE,0x1F\n");
    CHECK(r.has_device && r.device_id == 31u, "hex");
    r = scan_s("#DEVICE,4294967294\n");
    CHECK(r.has_device && r.device_id == 4294967294u, "the largest");
    r = scan_s("#DEVICE,4294967295\n");
    CHECK(!r.has_device && r.bad_directive, "all ones refused");
    r = scan_s("#DEVICE,4294967296\n");
    CHECK(!r.has_device && r.bad_directive, "33 bits refused");
    r = scan_s("#DEVICE,xyz\n");
    CHECK(!r.has_device && r.bad_directive && r.status == SETF_OK, "words ignored, not fatal");
    r = scan_s("#DEVICE,-5\n#DEVICE\n");
    CHECK(!r.has_device && r.bad_directive, "negative and missing ignored");

    /* ---- names ---- */
    r = scan_s("#MODULE,\n#LECTURE,\n");
    CHECK(r.has_module && r.has_lecture && r.module[0] == '\0' && r.lecture[0] == '\0', "empty values are still present");
    r = scan_s("#MODULE\n");
    CHECK(r.has_module && r.module[0] == '\0', "no comma at all is an empty value");
    r = scan_s("  #lecture ,  Spaced Out  \n");
    CHECK(r.has_lecture && strcmp(r.lecture, "Spaced Out") == 0, "keyword case, indent and padding [%s]", r.lecture);
    r = scan_s("#LECTURE,One\n#LECTURE,Two\n");
    CHECK(strcmp(r.lecture, "Two") == 0, "the last #LECTURE line wins");
    r = scan_s("#LECTURE,\"Intro, part 2\"\n");
    CHECK(strcmp(r.lecture, "Intro  part 2") == 0, "a comma inside quotes becomes a space [%s]", r.lecture);
    r = scan_s("#LECTURE,\"say \"\"hi\"\"\"\n");
    CHECK(strcmp(r.lecture, "say  hi") == 0, "doubled quotes [%s]", r.lecture);
    r = scan_s("#LECTURE,Part 1; Part 2 / Intro\n");
    CHECK(strcmp(r.lecture, "Part 1; Part 2 / Intro") == 0, "other punctuation is kept");
    r = scan_s("#LECTURE,A\tB\n");
    CHECK(strcmp(r.lecture, "A B") == 0, "a tab becomes a space");
    r = scan_s("#MODULE,abcdefghijklmnopqrstuvwxyz\n#LECTURE,abcdefghijklmnopqrstuvwxyz0123456789\n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuvwx") == 0 && strcmp(r.lecture, "abcdefghijklmnopqrstuvwxyz012345") == 0,
          "cut to 24 and 32 bytes [%s|%s]", r.module, r.lecture);
    r = scan_s("#MODULE,abcdefghijklmnopqrstuvwxy\n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuvwx") == 0, "one over the limit");
    r = scan_s("#MODULE,abcdefghijklmnopqrstuvwx\n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuvwx") == 0, "exactly the limit");
    r = scan_s("#MODULE,abcdefghijklmnopqrstuvw" "\xC3\xA9" "z\n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuvw") == 0, "a UTF-8 character split by the cut is dropped whole [%s]", r.module);
    r = scan_s("#MODULE,abcdefghijklmnopqrstuv" "\xC3\xA9" "z\n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuv" "\xC3\xA9") == 0, "one that fits is kept");
    r = scan_s("#LECTURE,\xE0\xB7\x83\xE0\xB7\x92\xE0\xB6\xB1\xE0\xB7\x8A\n");
    CHECK(strcmp(r.lecture, "\xE0\xB7\x83\xE0\xB7\x92\xE0\xB6\xB1\xE0\xB7\x8A") == 0, "Sinhala round trips");
    r = scan_s("#MODULE,abcdefghijklmnopqrstuvwx      \n");
    CHECK(strcmp(r.module, "abcdefghijklmnopqrstuvwx") == 0, "trailing spaces after a full name");

    /* ---- what is not a directive does nothing ---- */
    r = scan_s("CARD_ID,NAME\r\n1000,Alice\r\n#MODULE,M\r\n,,,\r\n");
    CHECK(r.status == SETF_OK && r.has_module && !r.has_lecture, "old student rows are ignored, directives still read");
    r = scan_s("#MODULES,x\n#LECTURES,y\n#TIMEZONE,z\n#DEVICES,1\n#NEWSESSIONS\n");
    CHECK(!r.has_module && !r.has_lecture && !r.has_time && !r.has_device && !r.new_session && !r.bad_directive,
          "longer keywords are not these directives");
    r = scan_s("1,#MODULE,x\n");
    CHECK(!r.has_module, "a # that is not first on the line is not a directive");
    r = scan_s("#THISKEYWORDISFARTOOLONGTOBEADIRECTIVE,x\n#MODULE,M\n");
    CHECK(r.has_module && strcmp(r.module, "M") == 0, "an over-long keyword does not derail the next line");
    r = scan_s("#NEWSESSION\n");
    CHECK(r.new_session, "#NEWSESSION needs no value");
    r = scan_s("#NEWSESSION,anything\n");
    CHECK(r.new_session, "or any value");

    /* ---- sizes ---- */
    r = scan_s("");
    CHECK(r.status == SETF_ERR_EMPTY, "an empty file");
    {
        char *big = (char *)malloc(SETF_MAX_BYTES + 100u);

        memset(big, ' ', SETF_MAX_BYTES + 100u);
        r = scan_n(big, SETF_MAX_BYTES + 1u);
        CHECK(r.status == SETF_ERR_TOO_LARGE, "oversize refused");
        memcpy(big, "#MODULE,Big\n", 12u);
        r = scan_n(big, SETF_MAX_BYTES);
        CHECK(r.status == SETF_OK && r.has_module, "exactly the limit is read");
        free(big);
    }

    /* ---- render, then parse it back ---- */
    n = setf_render(g_txt, sizeof(g_txt), &k_now, "EN2090", "Circuits Lecture 4", 12648430u);
    CHECK(n > 0u && n < 400u, "render fits easily (%u bytes)", n);
    g_txt[n] = '\0';
    CHECK(strncmp(g_txt, "# Edit these lines", 18u) == 0, "starts with the help line");
    CHECK(strstr(g_txt, "\r\n#TIME,2026-09-10 13:27:45\r\n") != NULL, "#TIME line");
    CHECK(strstr(g_txt, "\r\n#MODULE,EN2090\r\n#LECTURE,Circuits Lecture 4\r\n") != NULL, "#MODULE and #LECTURE lines");
    CHECK(strstr(g_txt, "\r\n#DEVICE,0012648430\r\n") != NULL, "#DEVICE line");
    CHECK(strstr(g_txt, "CARD_ID") == NULL, "no student rows: the device does not know students");
    CHECK(setf_render(g_txt, n - 1u, &k_now, "EN2090", "Circuits Lecture 4", 12648430u) == 0u,
          "a buffer one byte short is reported");
    r = scan_n(g_txt, n);
    CHECK(r.status == SETF_OK && r.has_time && r.has_device && r.device_id == 12648430u && !r.bad_directive &&
          strcmp(r.module, "EN2090") == 0 && strcmp(r.lecture, "Circuits Lecture 4") == 0 && !r.new_session,
          "what is shown parses back to what it says");

    n = setf_render(g_txt, sizeof(g_txt), &k_now, NULL, NULL, 0u);
    g_txt[n] = '\0';
    CHECK(strstr(g_txt, "#MODULE,\r\n#LECTURE,\r\n") != NULL && strstr(g_txt, "#DEVICE") == NULL,
          "no session and no device ID: blank slots to fill in, no #DEVICE");
    n = setf_render(g_txt, sizeof(g_txt), &k_now, "M,1", "L\"2\r\nX", 0u);
    g_txt[n] = '\0';
    CHECK(strstr(g_txt, "#MODULE,M 1\r\n#LECTURE,L 2  X\r\n") != NULL, "values are made safe on the way out");
    n = setf_render(g_txt, sizeof(g_txt), &k_now, "m", "l", 0xFFFFFFFEu);
    CHECK(n > 0u, "the largest device id renders");
}

/* ===================================================================== */
/* The USB volume                                                         */
/* ===================================================================== */

void test_usb_volume(void)
{
    static uint8_t big[FAT12_SECTOR_SIZE * 8u];
    static uint8_t one[FAT12_SECTOR_SIZE];
    static uint8_t file[4096];
    const uint32_t att = USBS_ATTEND_CLUSTER;
    char expect[96];
    uint32_t i;
    int32_t len;

    printf("usb volume\n");

    begin_session(100u, 0xC0FFEEu);

    /* 101 rows * 32 B = 3232 B = 7 clusters, after the metadata, STATUS.TXT,
     * the SETTINGS.CSV window and LASTCARD.TXT; then one cluster of
     * LECTURES.CSV, which with no markers in the log is just its header; then
     * the LECTURES folder (one cluster) holding L000, the same 7 clusters,
     * since with no lecture every tap came before the first one. */
    const uint32_t att_clusters = (csv_size(100u) + 511u) / 512u;
    const uint32_t all_clusters = 1u + USBS_SETTINGS_CLUSTERS + 1u + att_clusters + 1u + 1u + att_clusters;
    CHECK(att_clusters == 7u, "attend clusters %u", att_clusters);
    CHECK(usbs_file_size() == csv_size(100u), "file size %u", usbs_file_size());
    CHECK(usbs_sector_count() == FAT12_DATA_START_LBA + all_clusters,
          "sector count %u", usbs_sector_count());
    CHECK(usbs_sector_size() == 512u, "sector size");
    CHECK(att == 32u && USBS_LASTCARD_CLUSTER == 31u && USBS_SETTINGS_CLUSTERS == 28u, "cluster map");
    CHECK(FAT12_DATA_START_LBA == 26u && FAT12_SECTORS_PER_FAT == 12u, "FAT geometry");
    CHECK(usbs_lecture_file_count() == 1u, "one lecture file, L000");

    /* Boot sector: the fields a host validates. */
    CHECK(usbs_read(0u, g_sec, 1u), "read boot sector");
    CHECK(g_sec[510] == 0x55u && g_sec[511] == 0xAAu, "boot signature");
    CHECK(fat12_rd16(&g_sec[11]) == 512u, "bytes/sector");
    CHECK(g_sec[13] == 1u && fat12_rd16(&g_sec[14]) == 1u && g_sec[16] == 2u, "cluster, reserved, fats");
    CHECK(fat12_rd16(&g_sec[17]) == 16u, "root entries");
    CHECK(fat12_rd16(&g_sec[19]) == usbs_sector_count(), "total sectors field");
    CHECK(g_sec[21] == 0xF8u, "media byte");
    CHECK(fat12_rd16(&g_sec[22]) == FAT12_SECTORS_PER_FAT, "sectors per fat");
    CHECK(fat12_rd32(&g_sec[39]) == 0xC0FFEEu, "serial is the device id");
    CHECK(memcmp(&g_sec[54], "FAT12   ", 8u) == 0, "fs type");
    CHECK(fat12_cluster_count(usbs_sector_count()) < 4085u, "cluster count is FAT12");
    CHECK(fat12_cluster_count(usbs_sector_count()) == all_clusters, "clusters %u",
          fat12_cluster_count(usbs_sector_count()));

    /* Root directory: label, STATUS, SETTINGS, ATTEND, LECTURES. */
    CHECK(g_hf.root[11] == FAT12_ATTR_VOLUME_ID && memcmp(g_hf.root, "ATTENDANCE ", 11u) == 0, "label");
    int es = hf_find(&g_hf, HF_STATUS);
    int eu = hf_find(&g_hf, HF_SETTINGS);
    int ea = hf_find(&g_hf, HF_ATTEND);
    int el = hf_find(&g_hf, HF_LECTURES);
    CHECK(es >= 0 && eu >= 0 && ea >= 0 && el >= 0, "all four files listed");
    CHECK(hf_find(&g_hf, "STUDENTSCSV") < 0, "there is no student file any more");
    CHECK(hf_first(&g_hf, es) == 2u && hf_size(&g_hf, es) == 512u, "STATUS entry");
    CHECK(hf_first(&g_hf, eu) == 3u && hf_size(&g_hf, eu) == usbs_settings_size(), "SETTINGS entry");
    CHECK(hf_first(&g_hf, ea) == att && hf_size(&g_hf, ea) == csv_size(100u), "ATTEND entry");
    CHECK(hf_first(&g_hf, el) == att + att_clusters && hf_size(&g_hf, el) == csv_lecture_size(0u), "LECTURES entry");
    CHECK((g_hf.root[el * 32 + 11] & FAT12_ATTR_READ_ONLY) != 0u, "LECTURES is read-only");
    CHECK((g_hf.root[es * 32 + 11] & FAT12_ATTR_READ_ONLY) != 0u, "STATUS is read-only");
    CHECK((g_hf.root[ea * 32 + 11] & FAT12_ATTR_READ_ONLY) != 0u, "ATTEND is read-only");
    CHECK((g_hf.root[eu * 32 + 11] & FAT12_ATTR_READ_ONLY) == 0u, "SETTINGS is writable");
    {
        int ec = hf_find(&g_hf, HF_LASTCARD);
        int ed = hf_find(&g_hf, HF_LECTDIR);

        CHECK(ec >= 0 && hf_first(&g_hf, ec) == USBS_LASTCARD_CLUSTER && hf_size(&g_hf, ec) == 512u &&
              (g_hf.root[ec * 32 + 11] & FAT12_ATTR_READ_ONLY) != 0u, "LASTCARD entry");
        CHECK(ed >= 0 && hf_first(&g_hf, ed) == att + att_clusters + 1u && hf_size(&g_hf, ed) == 0u &&
              g_hf.root[ed * 32 + 11] == FAT12_ATTR_DIRECTORY, "LECTURES folder entry");
    }
    CHECK(g_hf.root[7 * 32] == 0x00u, "directory ends after the seventh entry");

    /* FAT chains and free space. */
    uint32_t set_clusters = (usbs_settings_size() + 511u) / 512u;
    CHECK(set_clusters == 1u, "settings fit one cluster (%u bytes)", usbs_settings_size());
    CHECK(hf_chain_len(&g_hf, 2u) == 1u, "STATUS chain");
    CHECK(hf_chain_len(&g_hf, 3u) == set_clusters, "SETTINGS chain");
    CHECK(hf_chain_len(&g_hf, att) == att_clusters, "ATTEND chain %u", hf_chain_len(&g_hf, att));
    CHECK(hf_free(&g_hf) == USBS_SETTINGS_CLUSTERS - set_clusters, "free space is the rest of the window (%u)", hf_free(&g_hf));
    CHECK(fat12_get(g_hf.fat, 0u) == 0x0FF8u && fat12_get(g_hf.fat, 1u) == 0x0FFFu, "reserved entries");
    for (i = 0u; i < FAT12_SECTORS_PER_FAT; i++) {
        CHECK(usbs_read(1u + i, g_sec, 1u) &&
              usbs_read(1u + FAT12_SECTORS_PER_FAT + i, one, 1u) &&
              memcmp(g_sec, one, 512u) == 0, "FAT copies match (sector %u)", i);
    }

    /* ATTEND.CSV: sixteen rows to a sector; row 0 is the header, record n is row n + 1. */
    CHECK(usbs_read(fat12_cluster_lba(att), g_sec, 1u), "read first data sector");
    snprintf(expect, sizeof(expect), "%-30s\r\n", "DATE,TIME,CARD_ID");
    CHECK(memcmp(g_sec, expect, 32u) == 0, "csv header");
    CHECK(memcmp(&g_sec[32], "2026-09-10,13:27:45,0000001000\r\n", 32u) == 0, "first record row [%.32s]", &g_sec[32]);
    CHECK(memcmp(&g_sec[15u * 32u], "2026-09-10,13:27:59,0000001028\r\n", 32u) == 0, "row 15 is the last of sector 0 [%.32s]", &g_sec[480]);
    CHECK(usbs_read(fat12_cluster_lba(att) + 1u, g_sec, 1u), "read second data sector");
    CHECK(memcmp(g_sec, "2026-09-10,13:28:00,0000001035\r\n", 32u) == 0, "sector 1 starts at record 15 [%.32s]", g_sec);
    CHECK(usbs_file_was_read(), "reading the file is noticed");

    /* The last sector: 101 rows = 6 full sectors and 5 rows; zeros after. */
    CHECK(usbs_read(fat12_cluster_lba(att) + 6u, g_sec, 1u), "read last sector");
    const uint32_t rows_in_last = 101u - (6u * 16u);
    CHECK(rows_in_last == 5u && g_sec[(rows_in_last - 1u) * 32u] == '2', "last row present");
    uint32_t nz = 0u;
    for (i = rows_in_last * 32u; i < 512u; i++) { nz += (g_sec[i] != 0u); }
    CHECK(nz == 0u, "bytes past the end are zero");

    /* Whole file, row by row, against what the log says. */
    uint32_t bad = 0u;
    for (i = 0u; i < 100u; i++) {
        uint32_t row = i + 1u;
        char got[CSV_ROW_BYTES];
        app_record_t rec;
        app_datetime_t t;

        (void)usbs_read(fat12_cluster_lba(att) + (row / CSV_ROWS_PER_SECTOR), one, 1u);
        memcpy(got, &one[(row % CSV_ROWS_PER_SECTOR) * CSV_ROW_BYTES], CSV_ROW_BYTES);
        (void)log_read(&g_ls, i, &rec);
        time_from_epoch(rec.stamp, &t);
        snprintf(expect, sizeof(expect), "%04u-%02u-%02u,%02u:%02u:%02u,%010u\r\n", t.year, t.month, t.day,
                 t.hour, t.minute, t.second, rec.student_id);
        if (memcmp(got, expect, CSV_ROW_BYTES) != 0) { bad++; }
    }
    CHECK(bad == 0u, "%u rows differ from the log", bad);

    /* A multi-sector read equals the sectors read one at a time. */
    CHECK(usbs_read(fat12_cluster_lba(att), big, 6u), "6 sector read");
    bad = 0u;
    for (i = 0u; i < 6u; i++) {
        (void)usbs_read(fat12_cluster_lba(att) + i, one, 1u);
        bad += (memcmp(one, &big[i * 512u], 512u) != 0);
    }
    CHECK(bad == 0u, "multi-sector read matches");

    /* SETTINGS.CSV as the host reads it, by following its chain. */
    len = hf_read(&g_hf, HF_SETTINGS, file, sizeof(file) - 1u);
    CHECK(len == (int32_t)usbs_settings_size(), "SETTINGS length %d", (int)len);
    file[len] = 0u;
    CHECK(strstr((char *)file, "\r\n#TIME,2026-09-10 13:27:45\r\n") != NULL, "#TIME shown");
    CHECK(strstr((char *)file, "\r\n#MODULE,\r\n#LECTURE,\r\n") != NULL, "empty session slots to fill in");
    CHECK(strstr((char *)file, "\r\n#DEVICE,0012648430\r\n") != NULL, "device id shown");

    /* STATUS.TXT. */
    len = hf_read(&g_hf, HF_STATUS, file, sizeof(file) - 1u);
    CHECK(len == 512, "STATUS is one sector");
    file[len] = 0u;
    CHECK(strstr((char *)file, "Device ID    : 0012648430") != NULL, "status device id");
    CHECK(strstr((char *)file, "Clock        : 2026-09-10 13:27:45") != NULL, "status clock");
    CHECK(strstr((char *)file, "Attendance   : 100 records") != NULL, "status records");
    CHECK(strstr((char *)file, "Last tap     : 0000001063 at 2026-09-10 13:29:24") != NULL, "status last tap");
    CHECK(strstr((char *)file, "Lecture      : none set") != NULL, "status lecture");
    CHECK(strstr((char *)file, "SETTINGS.CSV : unchanged") != NULL, "status says unchanged");
    CHECK(strstr((char *)file, "Students") == NULL, "no student count: the device does not know any");
    CHECK(file[510] == '\r' && file[511] == '\n', "status ends in CRLF");

    /* Writes: what is allowed and what is not. */
    memset(one, 0xA5, sizeof(one));
    CHECK(!usbs_write(fat12_cluster_lba(att), one, 1u), "ATTEND.CSV is read-only");
    CHECK(!usbs_write(fat12_cluster_lba(att) + 6u, one, 1u), "end of ATTEND.CSV is read-only");
    CHECK(!usbs_write(fat12_cluster_lba(2u), one, 1u), "STATUS.TXT is read-only");
    CHECK(usbs_write(0u, one, 1u), "boot sector write accepted");
    CHECK(usbs_read(0u, g_sec, 1u) && g_sec[510] == 0x55u && g_sec[0] == 0xEBu, "...and ignored");
    CHECK(!usbs_write(usbs_sector_count(), one, 1u), "write past the volume refused");
    CHECK(!usbs_write(usbs_sector_count() - 1u, big, 2u), "write straddling the end refused");
    CHECK(!usbs_read(usbs_sector_count(), g_sec, 1u), "read past the volume fails");
    CHECK(!usbs_read(usbs_sector_count() - 1u, big, 2u), "read straddling the end fails");
    CHECK(usbs_read(usbs_sector_count() - 1u, g_sec, 1u), "last sector reads");
    CHECK(usbs_write(fat12_cluster_lba(8u), one, 1u), "window sector write accepted");
    CHECK(usbs_read(fat12_cluster_lba(8u), g_sec, 1u) && memcmp(g_sec, one, 512u) == 0, "...and reads back");
    CHECK(usbs_write(fat12_cluster_lba(3u), big, 4u), "four-sector window write");
    CHECK(!usbs_write(fat12_cluster_lba(USBS_ATTEND_CLUSTER - 2u), big, 4u), "a write running into ATTEND.CSV is refused");

    /* An empty log still exports a valid header-only file. */
    begin_session(0u, 0xC0FFEEu);
    CHECK(usbs_file_size() == CSV_ROW_BYTES, "header only");
    CHECK(usbs_sector_count() == FAT12_DATA_START_LBA + 1u + USBS_SETTINGS_CLUSTERS + 1u + 1u + 1u + 1u,
          "one ATTEND cluster, LECTURES.CSV, an empty LECTURES folder");
    CHECK(usbs_read(fat12_cluster_lba(att), g_sec, 1u), "read");
    snprintf(expect, sizeof(expect), "%-30s\r\n", "DATE,TIME,CARD_ID");
    CHECK(memcmp(g_sec, expect, 32u) == 0, "header present");
    CHECK(g_sec[32] == 0u, "no phantom rows");
    len = hf_read(&g_hf, HF_STATUS, file, 511u);
    file[(len < 0) ? 0 : len] = 0u;
    CHECK(strstr((char *)file, "Attendance   : 0 records") != NULL && strstr((char *)file, "Last tap     : none yet") != NULL,
          "status of an empty log");

    /* No device ID: the default serial, and no #DEVICE line. */
    begin_session(3u, 0u);
    CHECK(usbs_read(0u, g_sec, 1u) && fat12_rd32(&g_sec[39]) == 0x43415331u, "default serial");
    len = hf_read(&g_hf, HF_SETTINGS, file, sizeof(file) - 1u);
    file[(len < 0) ? 0 : len] = 0u;
    CHECK(len > 0 && strstr((char *)file, "#DEVICE") == NULL, "no #DEVICE line without an id");
    len = hf_read(&g_hf, HF_STATUS, file, 511u);
    file[(len < 0) ? 0 : len] = 0u;
    CHECK(strstr((char *)file, "Device ID") == NULL, "and no Device ID line in the status");
}

/* ===================================================================== */
/* A host edits SETTINGS.CSV                                              */
/* ===================================================================== */

void test_usb_settings(void)
{
    usbs_result_t r;
    device_cfg_t cfg;
    uint32_t w0, e0, sets0;
    app_datetime_t dt;

    printf("usb settings\n");

    /* ---- untouched session: nothing is applied ---- */
    begin_session(20u, 0xC0FFEEu);
    w0 = host_flash_writes; e0 = host_flash_erases; sets0 = host_rtc_sets;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE, "no change, nothing applied");
    CHECK(host_flash_writes == w0 && host_flash_erases == e0 && host_rtc_sets == sets0, "flash and clock untouched");

    /* Only the host updating a timestamp in the directory is not an edit. */
    begin_session(20u, 0xC0FFEEu);
    w0 = host_flash_writes; e0 = host_flash_erases;
    g_hf.root[hf_find(&g_hf, HF_ATTEND) * 32 + 18] ^= 0x01u;     /* last access date */
    g_hf.root[hf_find(&g_hf, HF_SETTINGS) * 32 + 18] ^= 0x01u;
    CHECK(hf_flush_root(&g_hf), "touch the directory");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE && host_flash_writes == w0 && host_flash_erases == e0, "timestamp-only change ignored");

    /* ---- the clock ---- */
    begin_session(20u, 0xC0FFEEu);
    shown_file();
    edit_text("2026-09-10 13:27:45", "2030-05-06 07:08:09");
    CHECK(hf_edit_inplace(&g_hf, HF_SETTINGS, g_txt, (uint32_t)strlen(g_txt)), "edit #TIME in place");
    sets0 = host_rtc_sets;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && r.time_set && host_rtc_sets == sets0 + 1u, "clock set");
    dt = rtc_now();
    CHECK(dt.year == 2030u && dt.month == 5u && dt.day == 6u && dt.hour == 7u && dt.minute == 8u && dt.second == 9u,
          "RTC holds the typed time");
    CHECK(!r.device_set && !r.session_start, "and nothing else changed");

    /* An unedited #TIME must not wind the clock back to the moment of attach. */
    begin_session(20u, 0xC0FFEEu);
    {
        app_datetime_t later = { 2026u, 9u, 10u, 15u, 0u, 0u };

        host_set_time(&later);              /* two hours pass while the host works */
        shown_file();
        strcat(g_txt, "# a note the user added\r\n");
        CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, (uint32_t)strlen(g_txt)), "host saves the file with a comment added");
        sets0 = host_rtc_sets;
        r = end_session();
        CHECK(r.outcome == USBS_IMPORT_OK && !r.time_set && host_rtc_sets == sets0, "unedited #TIME leaves the clock alone");
        CHECK(rtc_now().hour == 15u, "clock still 15:00");
    }

    /* A malformed #TIME or #DEVICE is reported and ignored; the rest still applies. */
    begin_session(20u, 0xC0FFEEu);
    CHECK(put_settings("#TIME,10/06/2030 7:08\r\n#DEVICE,banana\r\n#MODULE,EN2090\r\n"), "bad values");
    sets0 = host_rtc_sets;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && !r.time_set && !r.device_set && r.rep.bad_directive && host_rtc_sets == sets0,
          "bad values reported, not applied");
    CHECK(r.session_start && strcmp(r.module, "EN2090") == 0, "but the module still is");

    /* ---- the device ID ---- */
    begin_session(5u, 0xC0FFEEu);
    shown_file();
    edit_text("0012648430", "0000000042");
    CHECK(hf_edit_inplace(&g_hf, HF_SETTINGS, g_txt, (uint32_t)strlen(g_txt)), "edit #DEVICE");
    e0 = host_flash_erases;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && r.device_set && r.device_id == 42u, "device id changed");
    CHECK(host_flash_erases - e0 == 1u, "one page erased: just the config page");
    devcfg_load(&cfg);
    CHECK(cfg.valid && cfg.device_id == 42u, "and it reads back from flash");

    begin_session(5u, 0u);
    CHECK(put_settings("#DEVICE,0x1F\r\n"), "an id on a device that had none");
    r = end_session();
    devcfg_load(&cfg);
    CHECK(r.device_set && cfg.valid && cfg.device_id == 31u, "set from nothing");

    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("#DEVICE,12648430\r\n"), "the same id again");
    e0 = host_flash_erases;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && !r.device_set && host_flash_erases == e0, "an unchanged id is not rewritten");

    /* A failed flash write refuses the lot: no clock change, no lecture. */
    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("#TIME,2031-01-01 00:00:00\r\n#DEVICE,77\r\n#MODULE,M\r\n#LECTURE,L\r\n"), "settings with an id");
    host_fail_writes_after(1u);
    sets0 = host_rtc_sets;
    r = end_session();
    host_write_failures = 0u;
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FLASH, "flash failure reported");
    CHECK(!r.time_set && !r.session_start && host_rtc_sets == sets0, "and nothing else was applied");

    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("#DEVICE,77\r\n"), "an id the flash will store wrongly");
    host_corrupt_write_after(0u);
    r = end_session();
    host_corrupt_write = 0u;
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FLASH, "a silently wrong write is refused");

    /* ---- files that are not what they should be ---- */
    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("#MODULE,EN2090\r\n"), "a settings file");
    CHECK(hf_delete(&g_hf, HF_SETTINGS), "then deleted");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE, "deleting the file is not a request to change anything");

    begin_session(5u, 0xC0FFEEu);
    CHECK(hf_create(&g_hf, HF_SETTINGS, "x", 0u), "a zero-length file");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_EMPTY, "refused");

    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("CARD_ID,NAME\r\n1000,Alice\r\n"), "an old-style student file under the new name");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && !r.time_set && !r.session_start && !r.device_set,
          "student rows are ignored; there is nothing to apply");

    begin_session(5u, 0xC0FFEEu);
    CHECK(hf_create(&g_hf, "STUDENTSCSV", "1000,Alice\r\n", 11u), "a STUDENTS.CSV dropped onto the drive");
    CHECK(hf_create(&g_hf, "CLASS3  CSV", "1,A\r\n", 5u), "and another csv");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE, "other files are not the device's business");

    /* The ATTEND.CSV lines the host can reach are still the device's alone. */
    begin_session(5u, 0xC0FFEEu);
    {
        static const char junk[] = "overwritten";

        CHECK(hf_delete(&g_hf, HF_ATTEND), "a host deletes ATTEND.CSV");
        CHECK(hf_create(&g_hf, "ATTEND  CSV", junk, (uint32_t)strlen(junk)), "and writes its own");
        r = end_session();
        CHECK(r.outcome == USBS_IMPORT_NONE, "the log is not the host's to change, and nothing was applied");
    }

    /* ---- Windows-style replace and what Windows and macOS scatter around ---- */
    begin_session(5u, 0xC0FFEEu);
    {
        static const char svi[512] = "{GUID}";
        int e;

        CHECK(hf_create(&g_hf, "SYSTEM~1   ", svi, 512u), "System Volume Information directory");
        e = hf_find(&g_hf, "SYSTEM~1   ");
        g_hf.root[e * 32 + 11] = FAT12_ATTR_DIRECTORY | FAT12_ATTR_HIDDEN | FAT12_ATTR_SYSTEM;
        CHECK(hf_flush_root(&g_hf), "set directory attributes");
        CHECK(hf_create(&g_hf, "WPSETTIDAT ", svi, 12u), "WPSettings.dat");
        CHECK(hf_create(&g_hf, ".FSEVENT   ", svi, 20u), "macOS marker");

        uint8_t *d = &g_hf.root[10 * 32];
        memset(d, 0, 32u);
        memcpy(d, "SETTINGSCSV", 11u);
        d[11] = 0x0F;                       /* a long-file-name entry */
        CHECK(hf_flush_root(&g_hf), "long file name entry");
        CHECK(put_settings("#MODULE,EN2090\r\n#LECTURE,L1\r\n"), "now the settings");
    }
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && r.session_start && strcmp(r.lecture, "L1") == 0, "junk around the file is ignored");

    /* ---- the file lands in scattered clusters ---- */
    begin_session(5u, 0xC0FFEEu);
    {
        uint32_t n = 0u, k;

        CHECK(hf_delete(&g_hf, HF_SETTINGS), "free the cluster");
        CHECK(hf_create(&g_hf, "J1      DAT", "a", 1u), "J1");
        CHECK(hf_create(&g_hf, "J2      DAT", "b", 1u), "J2");
        CHECK(hf_create(&g_hf, "J3      DAT", "c", 1u), "J3");
        CHECK(hf_delete(&g_hf, "J2      DAT"), "leave a one-cluster hole");
        /* A file of comments that pushes the real lines into later clusters. */
        for (k = 0u; k < 60u; k++) {
            n += (uint32_t)snprintf(&g_txt[n], sizeof(g_txt) - n, "# padding line %02u of the file\r\n", k);
        }
        n += (uint32_t)snprintf(&g_txt[n], sizeof(g_txt) - n, "#MODULE,Scattered\r\n#LECTURE,Across clusters\r\n");
        CHECK(n > 2u * 512u, "the file spans three clusters (%u bytes)", n);
        CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "write fragmented");
        int e = hf_find(&g_hf, HF_SETTINGS);
        CHECK(hf_first(&g_hf, e) == 4u && fat12_get(g_hf.fat, 4u) == 6u, "chain really is fragmented");
        r = end_session();
        CHECK(r.outcome == USBS_IMPORT_OK && strcmp(r.module, "Scattered") == 0 && strcmp(r.lecture, "Across clusters") == 0,
              "fragmented file read in order");
    }

    /* ---- a host that writes in an unhelpful order ---- */
    begin_session(5u, 0xC0FFEEu);
    {
        /* Directory entry first, FAT second, data last and backwards, over a
         * chain that runs 8 -> 7 -> 6: nothing about the file is ascending.
         * File sector k therefore lives in cluster 8 - k. */
        uint8_t sec[512];
        const char line[] = "#LECTURE,Backwards writer\r\n";
        uint32_t size = (3u * 512u) - 40u, k, n = 0u;

        CHECK(hf_delete(&g_hf, HF_SETTINGS), "delete");
        fat12_dirent(&g_hf.root[4 * 32], HF_SETTINGS, FAT12_ATTR_ARCHIVE, 8u, size, 0x5A21u, 0x7000u);
        CHECK(hf_flush_root(&g_hf), "dirent first");
        fat12_set(g_hf.fat, 8u, 7u);
        fat12_set(g_hf.fat, 7u, 6u);
        fat12_set(g_hf.fat, 6u, FAT12_EOC);
        CHECK(hf_flush_fat(&g_hf), "FAT second");

        memset(g_txt, ' ', 3u * 512u);
        for (k = 0u; (n + sizeof(line)) < size; k++) {
            if ((k % 3u) == 2u) {
                memcpy(&g_txt[n], line, sizeof(line) - 1u);
                n += (uint32_t)sizeof(line) - 1u;
            } else {
                memcpy(&g_txt[n], "# padding padding padding padding\r\n", 35u);
                n += 35u;
            }
        }
        for (k = 3u; k > 0u; k--) {
            memcpy(sec, &g_txt[(k - 1u) * 512u], 512u);
            CHECK(usbs_write(fat12_cluster_lba(8u - (k - 1u)), sec, 1u), "write sector %u", k - 1u);
        }
    }
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK && strcmp(r.lecture, "Backwards writer") == 0,
          "out-of-order writes read in file order (%d)", (int)r.outcome);

    /* ---- the volume is full ---- */
    begin_session(5u, 0xC0FFEEu);
    {
        static uint8_t filler[26u * 512u];
        uint32_t n = 0u, k;

        memset(filler, 'x', sizeof(filler));
        CHECK(hf_create(&g_hf, "FILLER  DAT", filler, sizeof(filler)), "fill most of the window");
        for (k = 0u; k < 200u; k++) {
            n += (uint32_t)snprintf(&g_txt[n], sizeof(g_txt) - n, "# a long comment to take up space %03u\r\n", k);
        }
        CHECK(n > 2u * 512u, "needs more than the space left (%u bytes)", n);
        CHECK(!hf_create(&g_hf, HF_SETTINGS, g_txt, n), "host reports disk full");
    }
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE, "nothing applied after disk full");

    /* ---- the largest file the window holds ---- */
    begin_session(5u, 0xC0FFEEu);
    {
        uint32_t n = 0u;

        while ((n + 40u) < SETF_MAX_BYTES - 40u) {
            n += (uint32_t)snprintf(&g_txt[n], 80u, "# padding to fill the window exactly....\r\n");
        }
        n += (uint32_t)snprintf(&g_txt[n], 80u, "#MODULE,Last line\r\n");
        CHECK(n <= SETF_MAX_BYTES, "built a file of %u bytes", n);
        CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "the window holds it");
        r = end_session();
        CHECK(r.outcome == USBS_IMPORT_OK && strcmp(r.module, "Last line") == 0, "and the last line is read");
    }

    /* ---- STATUS.TXT says what will happen ---- */
    {
        static char st[513];
        int32_t n;

        begin_session(5u, 0xC0FFEEu);
        CHECK(put_settings("#TIME,2031-01-01 00:00:00\r\n#DEVICE,77\r\n#MODULE,EN2090\r\n"), "settings to announce");
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u);
        st[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(st, "OK, will be applied when you eject the drive, press the button or unplug") != NULL, "status: ok [%s]", st);
        CHECK(strstr(st, "clock will be set") != NULL, "status: clock");
        CHECK(strstr(st, "device ID will change") != NULL, "status: device id");
        CHECK(strstr(st, "a new lecture will start") != NULL, "status: lecture");

        CHECK(put_settings("# only a comment\r\n"), "a comment-only file");
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u);
        st[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(st, "nothing to change") != NULL, "status: nothing to change [%s]", st);

        CHECK(put_settings("#TIME,garbage\r\n"), "a bad value");
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u);
        st[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(st, "a #TIME or #DEVICE value was ignored") != NULL, "status: ignored value");

        CHECK(hf_delete(&g_hf, HF_SETTINGS), "delete");
        n = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u);
        st[(n < 0) ? 0 : n] = '\0';
        CHECK(strstr(st, "not found, the device keeps its settings") != NULL, "status: deleted");
    }

    /* ---- a session ends once ---- */
    begin_session(5u, 0xC0FFEEu);
    CHECK(put_settings("#MODULE,M\r\n"), "stage");
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_OK, "first end applies");
    w0 = host_flash_writes;
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_NONE && host_flash_writes == w0, "second end does nothing");
    CHECK(!usbs_write(fat12_cluster_lba(5u), g_sec, 1u), "writes refused once the session is over");
}

/* ===================================================================== */
/* Hostile or broken file systems                                         */
/* ===================================================================== */

/** Plant a SETTINGS.CSV directory entry with arbitrary fields. */
static void plant_settings(uint32_t first, uint32_t size)
{
    int e;

    (void)hf_delete(&g_hf, HF_SETTINGS);
    for (e = 0; e < (int)FAT12_ROOT_ENTRIES; e++) {
        if (g_hf.root[e * 32] == 0x00u || g_hf.root[e * 32] == 0xE5u) { break; }
    }
    fat12_dirent(&g_hf.root[e * 32], HF_SETTINGS, FAT12_ATTR_ARCHIVE, (uint16_t)first, size, 0x5A21u, 0x7000u);
    (void)hf_flush_root(&g_hf);
}

void test_usb_robustness(void)
{
    usbs_result_t r;
    uint8_t one[FAT12_SECTOR_SIZE];

    printf("usb robustness\n");

    memset(one, ' ', sizeof(one));
    memcpy(one, "#MODULE,Planted\r\n", 17u);

    /* A chain that loops back on itself. */
    begin_session(5u, 0xC0FFEEu);
    (void)usbs_write(fat12_cluster_lba(5u), one, 1u);
    (void)usbs_write(fat12_cluster_lba(6u), one, 1u);
    fat12_set(g_hf.fat, 5u, 6u);
    fat12_set(g_hf.fat, 6u, 5u);
    (void)hf_flush_fat(&g_hf);
    plant_settings(5u, 3u * 512u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "looping chain refused");
    CHECK(!r.session_start && !r.time_set && !r.device_set, "and nothing applied");

    /* A chain that wanders into ATTEND.CSV's clusters. */
    begin_session(5u, 0xC0FFEEu);
    plant_settings(USBS_ATTEND_CLUSTER, 512u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "first cluster outside the window");
    begin_session(5u, 0xC0FFEEu);
    (void)usbs_write(fat12_cluster_lba(5u), one, 1u);
    fat12_set(g_hf.fat, 5u, USBS_ATTEND_CLUSTER);
    (void)hf_flush_fat(&g_hf);
    plant_settings(5u, 1024u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "chain leaves the window");

    /* A chain that ends before the size says it should. */
    begin_session(5u, 0xC0FFEEu);
    fat12_set(g_hf.fat, 5u, FAT12_EOC);
    (void)hf_flush_fat(&g_hf);
    plant_settings(5u, 3u * 512u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "short chain refused");

    /* A free first cluster, the STATUS cluster, a zero size, an absurd size. */
    begin_session(5u, 0xC0FFEEu);
    plant_settings(0u, 100u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "cluster 0 refused");
    begin_session(5u, 0xC0FFEEu);
    plant_settings(USBS_STATUS_CLUSTER, 100u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FILE, "the STATUS.TXT cluster refused");
    begin_session(5u, 0xC0FFEEu);
    plant_settings(5u, 0u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_EMPTY, "zero-length file refused");
    begin_session(5u, 0xC0FFEEu);
    fat12_set(g_hf.fat, 5u, FAT12_EOC);
    (void)hf_flush_fat(&g_hf);
    plant_settings(5u, 0xFFFFFFFFu);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_TOO_LARGE, "4 GB size refused");
    begin_session(5u, 0xC0FFEEu);
    plant_settings(5u, SETF_MAX_BYTES + 1u);
    r = end_session();
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_TOO_LARGE, "one byte over the window refused");

    /* A size that reaches the last cluster of the window and not beyond is fine. */
    begin_session(5u, 0xC0FFEEu);
    {
        uint32_t k;

        plant_settings(3u, 8u * 512u);      /* first: it frees the old file and rewrites the FAT */
        for (k = 0u; k < 8u; k++) {
            (void)usbs_write(fat12_cluster_lba(3u + k), one, 1u);
            fat12_set(g_hf.fat, 3u + k, (k == 7u) ? FAT12_EOC : (uint16_t)(4u + k));
        }
        (void)hf_flush_fat(&g_hf);
        r = end_session();
        CHECK(r.outcome == USBS_IMPORT_OK, "a file filling the whole window is read (%d)", (int)r.outcome);
    }

    /* A refusal never half-applies. */
    begin_session(5u, 0xC0FFEEu);
    {
        uint32_t e0 = host_flash_erases, w0 = host_flash_writes, s0 = host_rtc_sets;

        (void)usbs_write(fat12_cluster_lba(5u), one, 1u);
        fat12_set(g_hf.fat, 5u, 6u);
        fat12_set(g_hf.fat, 6u, 5u);
        (void)hf_flush_fat(&g_hf);
        plant_settings(5u, 3u * 512u);
        r = end_session();
        CHECK(host_flash_erases == e0 && host_flash_writes == w0 && host_rtc_sets == s0, "flash and clock untouched by every refusal");
    }

    /* The status file explains what is wrong, and what will happen. */
    {
        static uint8_t status[FAT12_SECTOR_SIZE + 1u];

        begin_session(5u, 0xC0FFEEu);
        plant_settings(USBS_ATTEND_CLUSTER, 512u);
        CHECK(hf_read(&g_hf, HF_STATUS, status, 512u) == 512, "status read");
        status[512] = 0u;
        CHECK(strstr((char *)status, "SETTINGS.CSV : ERROR, the file could not be read, copy it again. Nothing will be applied") != NULL,
              "status names the problem [%s]", (char *)status);
        plant_settings(5u, 0u);
        (void)hf_read(&g_hf, HF_STATUS, status, 512u); status[512] = 0u;
        CHECK(strstr((char *)status, "ERROR, the file is empty") != NULL, "status: empty");
        plant_settings(5u, SETF_MAX_BYTES + 1u);
        (void)hf_read(&g_hf, HF_STATUS, status, 512u); status[512] = 0u;
        CHECK(strstr((char *)status, "ERROR, the file is too large") != NULL, "status: too large");
        (void)end_session();
    }
}
