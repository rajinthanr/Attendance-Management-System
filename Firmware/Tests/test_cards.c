/**
 * @file    test_cards.c
 * @brief   Cards: the device keeps no registered list, records every card,
 *          shows a card tapped while plugged in to the PC (LASTCARD.TXT), and
 *          splits the log into one CSV per lecture (the LECTURES folder).
 */
#include "test_fsm_util.h"

#include "app_fsm.h"
#include "app_events.h"
#include "csv.h"
#include "device_cfg.h"
#include "log_store.h"
#include "settings_file.h"
#include "usb_storage.h"
#include "platform_if.h"

/* ---- helpers ------------------------------------------------------------ */

typedef struct {
    const char *s;
    uint32_t    n;
} mem_t;

static int mem_get(void *ctx, uint32_t off)
{
    const mem_t *m = (const mem_t *)ctx;

    return (off < m->n) ? (int)(uint8_t)m->s[off] : -1;
}

static const app_datetime_t k_now = { 2026u, 9u, 10u, 13u, 0u, 0u };

static log_store_t g_ls;
static hostfs_t    g_hf;
static char        g_txt[4096];

static void begin(const device_cfg_t *cfg)
{
    log_init(&g_ls);
    host_set_time(&k_now);
    usbs_begin(&g_ls, cfg, &k_now);
    hf_mount(&g_hf);
}

/** Read a root file as text. */
static const char *text_of(hostfs_t *h, const char *name11)
{
    int32_t n = hf_read(h, name11, (uint8_t *)g_txt, sizeof(g_txt) - 1u);

    g_txt[(n < 0) ? 0 : n] = '\0';
    return g_txt;
}

/** Hold card @p id to the reader until it is read, then take it away. */
static void hold_card(uint32_t id)
{
    const uint8_t uid[4] = { (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    uint32_t before = dbg_card_count;
    uint32_t i;

    host_card_set(uid, 4u, 0x08u);
    for (i = 0u; i < 3000u && dbg_card_count == before; i++) {
        app_task();
    }
    CHECK(dbg_card_count != before, "card %u was read", id);
    g_fb = host_out_mask;
    host_card_present = false;
    run_ms(1500u);
}

/** Change SETTINGS.CSV while plugged in, then unplug so it is applied. */
static void settings(const char *text, uint32_t at_s)
{
    static hostfs_t h;
    app_datetime_t dt = at(at_s);

    plug();
    hf_mount(&h);
    CHECK(hf_create(&h, HF_SETTINGS, text, (uint32_t)strlen(text)), "host writes SETTINGS.CSV");
    host_set_time(&dt);
    (void)unplug();
}

/* ===================================================================== */
/* No card list                                                           */
/* ===================================================================== */

static void test_cards_no_list(void)
{
    device_cfg_t c;
    usbs_result_t r;
    setf_report_t rep;
    uint32_t e0;

    printf("cards: no list on the device\n");

    host_flash_erase_all();
    devcfg_load(&c);
    begin(&c);
    CHECK(strstr(text_of(&g_hf, HF_SETTINGS), "#CARDS") == NULL, "SETTINGS.CSV shows no card list");
    text_of(&g_hf, HF_STATUS);
    CHECK(strstr(g_txt, "Cards") == NULL, "STATUS.TXT has no Cards line [%s]", g_txt);
    CHECK(strstr(g_txt, "Battery      : ") != NULL, "but a Battery line");
    usbs_end(&r);

    /* An older app still sends its list: a comment, and numbers to ignore. */
    begin(&c);
    {
        const char *old = "#MODULE,EN2090\r\n#CARDS,3\r\n0000000020\r\n0000000010\r\nten\r\n";

        CHECK(hf_create(&g_hf, HF_SETTINGS, old, (uint32_t)strlen(old)), "an old app's file");
        text_of(&g_hf, HF_STATUS);
        CHECK(strstr(g_txt, "ERROR") == NULL && strstr(g_txt, "a new lecture will start") != NULL,
              "accepted for its lecture [%s]", g_txt);
        e0 = host_flash_erases;
        usbs_end(&r);
        CHECK(r.outcome == USBS_IMPORT_OK && r.session_start && host_flash_erases == e0,
              "applied, and nothing erased for the list");
    }

    {
        const char *bad = "#CARDS,2\r\n20\r\n10\r\n";
        mem_t m = { bad, (uint32_t)strlen(bad) };

        setf_scan(mem_get, &m, m.n, &rep);
        CHECK(rep.status == SETF_OK && !rep.has_module, "an out-of-order list is no error any more");
    }

    /* A config page written by a firmware that kept a list still loads. */
    host_flash_erase_all();
    {
        const uint32_t words[8] = { NV_CONFIG_MAGIC, NV_CONFIG_FORMAT, 77u, 3u, 0x12345678u, 0u, 0u, 0u };

        memcpy(&host_flash[NV_CONFIG_OFFSET], words, sizeof(words));
    }
    devcfg_load(&c);
    CHECK(c.valid && c.device_id == 77u, "old config: the device ID is kept");
    CHECK(devcfg_set_device_id(&c, 78u) && c.device_id == 78u, "and can be changed");
    {
        nv_config_t raw;

        memcpy(&raw, &host_flash[NV_CONFIG_OFFSET], sizeof(raw));
        CHECK(raw.old_card_count == 0u && raw.old_card_crc32 == 0u, "the old list fields are cleared");
    }
}

/* ===================================================================== */
/* The tap                                                                */
/* ===================================================================== */

static void test_cards_tap(void)
{
    static hostfs_t h;
    static char csv[4096];

    printf("cards: every card is recorded\n");

    boot_fresh();
    /* Whatever an older firmware left where its list was is ignored. */
    memset(&host_flash[NV_PAGE_SIZE], 0x5A, NV_PAGE_SIZE);
    reboot();

    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED && (g_fb & FB_GREEN_BIT) != 0u && (g_fb & FB_RED_BIT) == 0u,
          "a card: green and a buzz");
    CHECK(tap(999u, 20u) == APP_SCAN_ACCEPTED && (g_fb & FB_RED_BIT) == 0u, "any other card: green too");
    CHECK(tap(999u, 30u) == APP_SCAN_DUPLICATE, "a card again is a duplicate");

    plug();
    read_attend(&h, csv, sizeof(csv));
    CHECK(strstr(csv, "2026-09-10,13:00:10,0000001000\r\n") != NULL, "first card logged");
    CHECK(strstr(csv, "2026-09-10,13:00:20,0000000999\r\n") != NULL, "second card logged");
    CHECK(rows_in(csv) == 2u, "two rows (%u)", rows_in(csv));
    (void)unplug();
}

/* ===================================================================== */
/* Registering a card at the PC                                           */
/* ===================================================================== */

static void test_cards_enrol(void)
{
    static hostfs_t h;
    static char csv[4096];

    printf("cards: registering while plugged in\n");

    boot_fresh();
    CHECK(tap(1000u, 10u) == APP_SCAN_ACCEPTED, "a tap before the cable");

    plug();
    hf_mount(&h);
    CHECK(hf_find(&h, HF_LASTCARD) >= 0 && hf_size(&h, hf_find(&h, HF_LASTCARD)) == 512u, "LASTCARD.TXT, one sector");
    text_of(&h, HF_LASTCARD);
    CHECK(strncmp(g_txt, "LAST CARD\r\nTaps    : 0\r\nCard ID : none yet\r\nUID     : -\r\n", 55u) == 0,
          "nothing yet [%s]", g_txt);
    CHECK(g_txt[510] == '\r' && g_txt[511] == '\n' && g_txt[100] == ' ', "space padded, CRLF at the end");

    /* The reader works with the drive up, and a tap goes to the PC. */
    hold_card(0x0AF41A9Eu);
    CHECK(app_state() == ST_USB, "still a USB session");
    CHECK(dbg_scan_result == (uint8_t)APP_SCAN_ENROLLED, "shown to the PC (%u)", dbg_scan_result);
    CHECK((g_fb & FB_GREEN_BIT) != 0u && (g_fb & FB_VIB_BIT) != 0u, "green and a buzz");
    text_of(&h, HF_LASTCARD);
    CHECK(strstr(g_txt, "Taps    : 1\r\nCard ID : 0183769758\r\nUID     : 0A F4 1A 9E\r\n") != NULL,
          "the card, as the CSV writes it [%s]", g_txt);

    /* The same card again is counted again: the app is waiting for a tap. */
    hold_card(0x0AF41A9Eu);
    text_of(&h, HF_LASTCARD);
    CHECK(strstr(g_txt, "Taps    : 2\r\n") != NULL, "a second tap counts [%s]", g_txt);
    CHECK(usbs_last_card_taps() == 2u, "two taps this session");

    /* Not attendance. */
    read_attend(&h, csv, sizeof(csv));
    CHECK(rows_in(csv) == 1u, "nothing logged while plugged in (%u rows)", rows_in(csv));
    (void)unplug();

    /* Unplugged, the same card is attendance again. */
    CHECK(tap(0x0AF41A9Eu, 100u) == APP_SCAN_ACCEPTED, "after the cable, a tap is logged");
    plug();
    hf_mount(&h);
    text_of(&h, HF_LASTCARD);
    CHECK(strstr(g_txt, "Taps    : 0\r\n") != NULL, "the count starts again at each plug-in");
    (void)unplug();

    /* A charger: no host ever comes, so a card held meanwhile is attendance. */
    {
        const uint8_t uid[4] = { 0u, 0u, 0x07u, 0xD0u };
        uint32_t before = dbg_card_count;

        run_ms(2000u);
        host_vbus = true;
        host_usb_configured = false;
        run_ms(1000u);
        CHECK(app_state() == ST_USB, "waiting to see whether a host enumerates");
        host_card_set(uid, 4u, 0x08u);
        run_ms(1000u);
        CHECK(dbg_card_count == before, "no host yet: the card is not taken for registering");
        run_ms(APP_USB_ENUM_TIMEOUT_MS);
        CHECK(app_state() == ST_IDLE && dbg_card_count == before + 1u &&
              dbg_scan_result == (uint8_t)APP_SCAN_ACCEPTED, "a charger after all: the card is logged (%u)",
              dbg_scan_result);
        host_card_present = false;
        host_vbus = false;
        run_ms(1500u);
    }
}

/* ===================================================================== */
/* One CSV per lecture                                                    */
/* ===================================================================== */

static void test_cards_lectures(void)
{
    static hostfs_t h;
    static hf_dirent_t d[8];
    static uint8_t buf[2048];
    int n;
    int32_t got;

    printf("cards: the LECTURES folder\n");

    boot_fresh();
    plug();
    hf_mount(&h);
    n = hf_list(&h, HF_LECTDIR, d, 8u);
    CHECK(n == 0, "an empty log: an empty folder (%d)", n);
    (void)unplug();

    CHECK(tap(1000u, 5u) == APP_SCAN_ACCEPTED, "a tap before any lecture");
    settings("#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n", 60u);          /* 13:01:00 */
    CHECK(tap(1000u, 70u) == APP_SCAN_ACCEPTED, "lecture 1: first card");
    CHECK(tap(1007u, 80u) == APP_SCAN_ACCEPTED, "lecture 1: second card");
    settings("#NEWSESSION,1\r\n", 3600u + 120u);                        /* 14:02:00 */
    CHECK(tap(1007u, 3600u + 130u) == APP_SCAN_ACCEPTED, "lecture 2: a card");
    settings("#NEWSESSION,1\r\n", 7200u);                               /* 15:00:00, nobody came */

    plug();
    hf_mount(&h);
    n = hf_list(&h, HF_LECTDIR, d, 8u);
    CHECK(n == 4, "four files (%d)", n);
    if (n == 4) {
        CHECK(strcmp(d[0].name, "L000_2026-09-10_13-00.csv") == 0, "taps before the first lecture [%s]", d[0].name);
        CHECK(strcmp(d[1].name, "L001_2026-09-10_13-01.csv") == 0, "lecture 1 [%s]", d[1].name);
        CHECK(strcmp(d[2].name, "L002_2026-09-10_14-02.csv") == 0, "lecture 2 [%s]", d[2].name);
        CHECK(strcmp(d[3].name, "L003_2026-09-10_15-00.csv") == 0, "lecture 3 [%s]", d[3].name);
        CHECK(strcmp(d[1].sfn, "L001.CSV") == 0, "8.3 alias [%s]", d[1].sfn);
        CHECK((d[1].attr & FAT12_ATTR_READ_ONLY) != 0u, "read-only");
        CHECK(d[1].date == (uint16_t)(((2026u - 1980u) << 9) | (9u << 5) | 10u) &&
              d[1].time == (uint16_t)((13u << 11) | (1u << 5)), "dated when the lecture started");
        CHECK(d[0].size == 2u * CSV_ROW_BYTES && d[1].size == 3u * CSV_ROW_BYTES &&
              d[2].size == 2u * CSV_ROW_BYTES && d[3].size == CSV_ROW_BYTES, "header plus each lecture's rows");

        got = hf_read_chain(&h, d[1].first, d[1].size, buf, sizeof(buf) - 1u);
        buf[(got < 0) ? 0 : got] = '\0';
        CHECK(strncmp((char *)buf, "DATE,TIME,CARD_ID ", 18u) == 0 &&
              strstr((char *)buf, "2026-09-10,13:01:10,0000001000\r\n") != NULL &&
              strstr((char *)buf, "2026-09-10,13:01:20,0000001007\r\n") != NULL, "lecture 1's taps [%s]", buf);
        got = hf_read_chain(&h, d[2].first, d[2].size, buf, sizeof(buf) - 1u);
        buf[(got < 0) ? 0 : got] = '\0';
        CHECK(strstr((char *)buf, "2026-09-10,14:02:10,0000001007\r\n") != NULL &&
              strstr((char *)buf, "0000001000") == NULL, "lecture 2's only [%s]", buf);
    }
    text_of(&h, HF_STATUS);
    CHECK(strstr(g_txt, "Lectures     : 4 files in the LECTURES folder") != NULL, "STATUS.TXT counts them [%s]", g_txt);

    /* A host writing the folder back (an access date) is not an error. */
    {
        uint8_t sec[FAT12_SECTOR_SIZE];
        int e = hf_find(&h, HF_LECTDIR);
        uint32_t lba = fat12_cluster_lba(hf_first(&h, e));

        CHECK(usbs_read(lba, sec, 1u), "read the folder");
        sec[64 + 18] ^= 0x01u;
        CHECK(usbs_write(lba, sec, 1u), "a write to the folder is accepted");
        CHECK(hf_list(&h, HF_LECTDIR, d, 8u) == 4 && strcmp(d[1].name, "L001_2026-09-10_13-01.csv") == 0,
              "and changes nothing");
    }
    (void)unplug();

    /* After a clear the numbers start from 1 again. */
    settings("#CLEARLOG,1\r\n#MODULE,EN2090\r\n#LECTURE,Lecture 9\r\n", 9000u);    /* 15:30:00 */
    CHECK(tap(1000u, 9010u) == APP_SCAN_ACCEPTED, "a tap in the new lecture");
    plug();
    hf_mount(&h);
    n = hf_list(&h, HF_LECTDIR, d, 8u);
    CHECK(n == 1 && strcmp(d[0].name, "L001_2026-09-10_15-30.csv") == 0, "one file, number 1 (%d) [%s]",
          n, (n > 0) ? d[0].name : "");
    (void)unplug();
}

void test_cards(void)
{
    test_cards_no_list();
    test_cards_tap();
    test_cards_enrol();
    test_cards_lectures();
}
