/**
 * @file    test_cards.c
 * @brief   The registered card list: IDs only, kept on the device so a tap can
 *          show green (known) or red (unknown). The CSV never carries a name;
 *          every tap is recorded whichever colour it gets.
 */
#include "test_util.h"
#include "test_hostfs.h"

#include "app_fsm.h"
#include "app_events.h"
#include "csv.h"
#include "device_cfg.h"
#include "log_store.h"
#include "settings_file.h"
#include "usb_storage.h"
#include "platform_if.h"
#include "crc.h"

#define OK_FB    (PLAT_OUT_LED_GREEN | PLAT_OUT_VIBRATION)
#define UNK_FB   (PLAT_OUT_LED_RED | PLAT_OUT_VIBRATION)

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

static void scan(const char *text, setf_report_t *rep)
{
    mem_t m = { text, (uint32_t)strlen(text) };

    setf_scan(mem_get, &m, m.n, rep);
}

/** CRC the way the list stores it, computed independently of the parser. */
static uint32_t crc_of(const uint32_t *ids, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu, i;

    for (i = 0u; i < n; i++) {
        uint8_t b[4] = { (uint8_t)ids[i], (uint8_t)(ids[i] >> 8), (uint8_t)(ids[i] >> 16), (uint8_t)(ids[i] >> 24) };

        crc = crc32_ieee_update(crc, b, 4u);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct {
    const uint32_t *ids;
    uint32_t        n, i;
} feed_t;

static bool feed_next(void *ctx, uint32_t *id)
{
    feed_t *f = (feed_t *)ctx;

    if (f->i >= f->n) { return false; }
    *id = f->ids[f->i++];
    return true;
}

static bool store(device_cfg_t *c, uint32_t dev, const uint32_t *ids, uint32_t n)
{
    feed_t f = { ids, n, 0u };

    return devcfg_set_cards(c, dev, n, crc_of(ids, n), feed_next, &f);
}

static const app_datetime_t k_now = { 2026u, 9u, 10u, 13u, 0u, 0u };

/** Settings text with a card list: header lines plus @p n numbers. */
static uint32_t make_text(char *out, uint32_t cap, const uint32_t *ids, uint32_t n, uint32_t declared)
{
    uint32_t len = 0u, i;

    len += (uint32_t)snprintf(&out[len], cap - len, "#MODULE,EN2090\r\n#CARDS,%u\r\n", declared);
    for (i = 0u; i < n; i++) {
        len += (uint32_t)snprintf(&out[len], cap - len, "%010u\r\n", ids[i]);
    }
    return len;
}

/* ===================================================================== */
/* The parser                                                             */
/* ===================================================================== */

static void test_cards_parser(void)
{
    static const uint32_t ids[] = { 5u, 1000u, 1007u, 777777u, 4000000000u };
    setf_report_t r;
    static char big[32768];
    uint32_t i, n;

    printf("cards: parser\n");

    scan("#CARDS,5\r\n0000000005\r\n0000001000\r\n0000001007\r\n0000777777\r\n4000000000\r\n", &r);
    CHECK(r.status == SETF_OK && r.has_cards && r.card_count == 5u, "five cards (%d, %u)", (int)r.status, r.card_count);
    CHECK(r.card_crc == crc_of(ids, 5u), "the CRC matches an independent one");

    scan("#MODULE,EN2090\r\n", &r);
    CHECK(r.status == SETF_OK && !r.has_cards, "no #CARDS line: the list is left alone");
    scan("#CARDS,0\r\n", &r);
    CHECK(r.status == SETF_OK && r.has_cards && r.card_count == 0u, "#CARDS,0 clears the list");
    scan("1000\r\n2000\r\n#CARDS,1\r\n3000\r\n", &r);
    CHECK(r.status == SETF_OK && r.card_count == 1u, "lines before #CARDS are ignored");

    scan("#CARDS,3\n1000\n0x7D1\n\"3000\"\n", &r);
    CHECK(r.status == SETF_OK && r.card_count == 3u, "LF ends, hex and quoted numbers (%d)", (int)r.status);
    scan("#CARDS,2\r1000\r2000\r", &r);
    CHECK(r.status == SETF_OK && r.card_count == 2u, "lone CR ends");
    scan("\xEF\xBB\xBF#CARDS,1\r\n42\r\n", &r);
    CHECK(r.status == SETF_OK && r.card_count == 1u, "a byte-order mark is skipped");
    scan("#CARDS,2\r\n1000,Alice Perera,EN/21/001\r\n2000,Bob\r\n", &r);
    CHECK(r.status == SETF_OK && r.card_count == 2u, "text after a comma is ignored (a spreadsheet row)");
    scan("#cards,2\r\n10\r\n\r\n   \r\n20\r\n# comment\r\n", &r);
    CHECK(r.status == SETF_OK && r.card_count == 2u, "any case, blank lines and comments inside the list");
    scan("#CARDS,2\r\n1000\r\n1000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 3u, "a repeated number is refused, at line 3 (%u)", r.bad_card_line);
    scan("#CARDS,2\r\n2000\r\n1000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 3u, "descending order is refused");
    scan("#CARDS,1\r\n0\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 2u, "zero is not a card");
    scan("#CARDS,1\r\n4294967040\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "0xFFFFFF00 and up are marker ids, not cards");
    scan("#CARDS,1\r\n4294967295\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "the erased pattern is not a card");
    scan("#CARDS,1\r\n12ab\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 2u, "letters are refused");
    scan("#CARDS,2\r\nCARD_ID\r\n1000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 2u, "a header row inside the list is refused");
    scan("#CARDS,1\r\n99999999999\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "a number that overflows 32 bits is refused");
    scan("#CARDS,3\r\n1000\r\n2000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 0u, "fewer numbers than declared (a truncated copy)");
    scan("#CARDS,1\r\n1000\r\n2000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS && r.bad_card_line == 0u, "more numbers than declared");
    scan("#CARDS,1\r\n1000\r\n#CARDS,1\r\n2000\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "two lists in one file");
    scan("#CARDS,abc\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "a count that is not a number");
    scan("#CARDS,1001\r\n", &r);
    CHECK(r.status == SETF_ERR_CARDS, "a count over the limit");

    /* The limit itself: 1000 fits, 1001 does not, and it all fits the window. */
    {
        static uint32_t many[1001];

        for (i = 0u; i < 1001u; i++) { many[i] = 100000u + (i * 3u); }
        n = make_text(big, sizeof(big), many, 1000u, 1000u);
        scan(big, &r);
        CHECK(r.status == SETF_OK && r.card_count == 1000u, "1000 cards are accepted (%d)", (int)r.status);
        CHECK(n < SETF_MAX_BYTES, "and 1000 cards fit the USB window (%u of %u bytes)", n, SETF_MAX_BYTES);
        CHECK(r.card_crc == crc_of(many, 1000u), "with the right CRC");
        n = make_text(big, sizeof(big), many, 1001u, 1001u);
        scan(big, &r);
        CHECK(r.status == SETF_ERR_CARDS, "1001 are refused");
    }

    /* The iterator yields what the scan counted, in order. */
    {
        const char *t = "#MODULE,X\r\n1000\r\n#CARDS,3\r\n10\r\n20\r\n30\r\n#LECTURE,Y\r\n";
        mem_t m = { t, (uint32_t)strlen(t) };
        setf_card_iter_t it;
        uint32_t id, got = 0u, sum = 0u;

        setf_cards_begin(&it, mem_get, &m, m.n);
        while (setf_cards_next(&it, &id)) { got++; sum += id; }
        CHECK(got == 3u && sum == 60u, "the iterator walks the three numbers (%u, %u)", got, sum);
    }
}

/* ===================================================================== */
/* Flash                                                                  */
/* ===================================================================== */

static void test_cards_flash(void)
{
    static uint32_t many[NV_CARDS_MAX];
    static const uint32_t four[] = { 11u, 22u, 33u, 44u };
    static const uint32_t three[] = { 11u, 22u, 33u };
    device_cfg_t c, d;
    uint32_t i, id;

    printf("cards: flash\n");

    host_flash_erase_all();
    devcfg_load(&c);
    CHECK(c.card_count == 0u && cards_is_known(&c, 12345u), "blank flash: no list, every card is known");

    CHECK(store(&c, 4242u, four, 4u), "store four");
    CHECK(c.valid && c.card_count == 4u && c.device_id == 4242u, "state after storing");
    CHECK(cards_verify(&c), "the list verifies");
    devcfg_load(&d);
    CHECK(d.card_count == 4u && d.card_crc == crc_of(four, 4u) && d.device_id == 4242u, "and again after a reboot");
    for (i = 0u; i < 4u; i++) { CHECK(cards_read(&d, i, &id) && id == four[i], "entry %u", i); }
    CHECK(!cards_read(&d, 4u, &id), "past the end reads nothing");
    CHECK(cards_is_known(&d, 11u) && cards_is_known(&d, 44u) && cards_is_known(&d, 22u), "members are known");
    CHECK(!cards_is_known(&d, 10u) && !cards_is_known(&d, 12u) && !cards_is_known(&d, 45u) && !cards_is_known(&d, 0xFFFFFFFFu),
          "others are not");

    CHECK(store(&c, 4242u, three, 3u) && c.card_count == 3u && cards_verify(&c), "an odd count");
    CHECK(cards_is_known(&c, 33u) && !cards_is_known(&c, 44u), "the old fourth card is gone");
    CHECK(store(&c, 7u, four, 1u) && c.card_count == 1u && cards_is_known(&c, 11u) && !cards_is_known(&c, 22u), "a single card");

    /* Changing only the device ID keeps the list. */
    CHECK(store(&c, 5u, four, 4u), "restore four");
    CHECK(devcfg_set_device_id(&c, 99u), "new ID");
    devcfg_load(&d);
    CHECK(d.device_id == 99u && d.card_count == 4u && cards_verify(&d), "the list survives an ID change");

    /* An empty list means "no list". */
    CHECK(store(&c, 99u, four, 0u) && c.card_count == 0u && cards_is_known(&c, 31337u), "an empty list: every card is known");

    /* Full size. */
    for (i = 0u; i < NV_CARDS_MAX; i++) { many[i] = 1000u + (i * 7u); }
    CHECK(store(&c, 99u, many, NV_CARDS_MAX) && cards_verify(&c), "a thousand cards");
    CHECK(cards_is_known(&c, many[0]) && cards_is_known(&c, many[999]) && cards_is_known(&c, many[500]), "ends and middle");
    CHECK(!cards_is_known(&c, many[500] + 1u) && !cards_is_known(&c, 999u) && !cards_is_known(&c, many[999] + 1u), "and strangers");
    {
        uint32_t known = 0u;

        for (i = 0u; i < NV_CARDS_MAX; i++) { known += cards_is_known(&c, many[i]) ? 1u : 0u; }
        CHECK(known == NV_CARDS_MAX, "every stored card is found (%u)", known);
    }
    CHECK(!devcfg_set_cards(&c, 1u, NV_CARDS_MAX + 1u, 0u, feed_next, &(feed_t){ many, NV_CARDS_MAX, 0u }),
          "more than the limit is refused");

    /* A feed that runs dry stores nothing usable. */
    CHECK(!devcfg_set_cards(&c, 1u, 5u, 0u, feed_next, &(feed_t){ four, 4u, 0u }), "a short feed fails");
    devcfg_load(&d);
    CHECK(!d.valid && cards_is_known(&d, 31337u), "and leaves no config, so every card is known");

    /* Damage. */
    CHECK(store(&c, 5u, four, 4u), "stored again");
    host_flash[NV_CARDS_OFFSET + 2u] ^= 0x01u;
    devcfg_load(&d);
    CHECK(d.card_count == 4u && !cards_verify(&d), "a flipped bit fails the CRC");

    /* Power lost part-way: the config goes first, so what remains is "no config". */
    for (i = 0u; i < 6u; i++) {
        CHECK(store(&c, 5u, four, 4u), "stored for the power-cut test");
        host_fail_writes_after(i);
        CHECK(!store(&c, 6u, three, 3u), "write %u fails", i);
        host_write_failures = 0u;
        devcfg_load(&d);
        CHECK(!d.valid || (d.card_count == 3u && cards_verify(&d)), "after a cut at write %u: no config or a whole list", i);
        if (!d.valid) { CHECK(cards_is_known(&d, 31337u), "no config means every card is known"); }
    }

    /* A wrong bit in the list itself (not the config) is caught too. */
    host_flash_erase_all();
    host_corrupt_write_after(0u);
    CHECK(!store(&c, 5u, four, 4u), "a corrupted card word is detected");
    host_corrupt_write = 0u;

    /* A flash that stores the wrong bit is caught by the read-back. */
    host_flash_erase_all();
    host_corrupt_write_after(2u);
    CHECK(!store(&c, 5u, four, 4u), "a corrupted word is detected");
    host_corrupt_write = 0u;
}

/* ===================================================================== */
/* The USB file                                                           */
/* ===================================================================== */

static log_store_t     g_ls;
static hostfs_t        g_hf;
static char            g_txt[32768];

static void begin(const device_cfg_t *cfg)
{
    log_init(&g_ls);
    host_set_time(&k_now);
    usbs_begin(&g_ls, cfg, &k_now);
    hf_mount(&g_hf);
}

static uint32_t shown(void)
{
    int32_t n = hf_read(&g_hf, HF_SETTINGS, (uint8_t *)g_txt, sizeof(g_txt) - 1u);

    if (n < 0) { n = 0; }
    g_txt[n] = '\0';
    return (uint32_t)n;
}

static void test_cards_usb(void)
{
    static const uint32_t four[] = { 11u, 22u, 33u, 44u };
    static const uint32_t other[] = { 5u, 22u, 99u };
    static char st[513];
    static uint32_t many[NV_CARDS_MAX];
    device_cfg_t c, after;
    usbs_result_t r;
    uint32_t i, n, e0;
    int32_t sn;

    printf("cards: usb\n");

    host_flash_erase_all();
    devcfg_load(&c);
    begin(&c);
    shown();
    CHECK(strstr(g_txt, "#CARDS,0000\r\n") != NULL, "no list: #CARDS,0 is shown");
    sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
    CHECK(strstr(st, "Cards        : none registered") != NULL, "status: none registered [%s]", st);
    usbs_end(&r);
    CHECK(r.outcome == USBS_IMPORT_NONE, "looking changes nothing");

    /* The host sends a list. */
    begin(&c);
    n = make_text(g_txt, sizeof(g_txt), four, 4u, 4u);
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "the host copies a file with four cards");
    sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
    CHECK(strstr(st, "4 cards will be registered") != NULL, "status announces it [%s]", st);
    usbs_end(&r);
    CHECK(r.outcome == USBS_IMPORT_OK && r.cards_set && r.card_count == 4u && r.device_set, "applied (%d)", (int)r.outcome);
    devcfg_load(&after);
    CHECK(after.card_count == 4u && cards_verify(&after) && cards_is_known(&after, 33u) && !cards_is_known(&after, 34u), "stored");

    /* Next attach shows that list, and a copy of it is a no-op for flash. */
    begin(&after);
    shown();
    CHECK(strstr(g_txt, "#CARDS,0004\r\n0000000011\r\n0000000022\r\n0000000033\r\n0000000044\r\n") != NULL, "the list is shown back, ascending");
    sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
    CHECK(strstr(st, "Cards        : 4 registered (CRC ") != NULL, "status: 4 registered");
    e0 = host_flash_erases;
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, (uint32_t)strlen(g_txt)), "the host saves the same file again");
    usbs_end(&r);
    CHECK(r.outcome == USBS_IMPORT_OK && !r.cards_set && host_flash_erases == e0, "same list: nothing erased");

    /* Same size, one different card: still a change. */
    begin(&after);
    {
        static const uint32_t swapped[] = { 11u, 22u, 33u, 55u };

        n = make_text(g_txt, sizeof(g_txt), swapped, 4u, 4u);
        CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "the same number of cards, one different");
        usbs_end(&r);
        devcfg_load(&after);
        CHECK(r.cards_set && cards_is_known(&after, 55u) && !cards_is_known(&after, 44u), "replaced (a count match is not enough)");
    }
    begin(&after);
    n = make_text(g_txt, sizeof(g_txt), four, 4u, 4u);
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "back to the first list");
    usbs_end(&r);
    devcfg_load(&after);

    /* A different list replaces it, and keeps the device ID unless one is given. */
    begin(&after);
    n = make_text(g_txt, sizeof(g_txt), other, 3u, 3u);
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "a new list");
    usbs_end(&r);
    devcfg_load(&after);
    CHECK(r.cards_set && after.card_count == 3u && after.device_id == 0u && cards_is_known(&after, 99u) && !cards_is_known(&after, 11u),
          "replaced; the device ID is kept (%u)", after.device_id);

    /* The same file also changing the ID. */
    begin(&after);
    n = make_text(g_txt, sizeof(g_txt), four, 4u, 4u);
    n += (uint32_t)snprintf(&g_txt[n], sizeof(g_txt) - n, "#DEVICE,321\r\n");
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "a list and a new ID together");
    usbs_end(&r);
    devcfg_load(&after);
    CHECK(r.outcome == USBS_IMPORT_OK && after.device_id == 321u && after.card_count == 4u && r.device_id == 321u, "both stored");

    /* An ID change alone keeps the list. */
    begin(&after);
    CHECK(hf_create(&g_hf, HF_SETTINGS, "#DEVICE,654\r\n", 13u), "an ID only");
    usbs_end(&r);
    devcfg_load(&after);
    CHECK(after.device_id == 654u && after.card_count == 4u && cards_verify(&after), "the list is untouched");

    /* A file without #CARDS leaves the list alone (the app's lecture-only writes). */
    begin(&after);
    CHECK(hf_create(&g_hf, HF_SETTINGS, "#MODULE,EN2090\r\n#LECTURE,One\r\n", 30u), "a lecture only");
    e0 = host_flash_erases;
    usbs_end(&r);
    devcfg_load(&after);
    CHECK(r.outcome == USBS_IMPORT_OK && r.session_start && after.card_count == 4u && host_flash_erases == e0, "list kept, no erase");

    /* Refusals change nothing. */
    {
        static const char *bad[] = {
            "#CARDS,3\r\n10\r\n20\r\n",                  /* truncated */
            "#CARDS,2\r\n20\r\n10\r\n",                  /* not ascending */
            "#CARDS,2\r\n10\r\nten\r\n",                 /* not a number */
        };

        for (i = 0u; i < 3u; i++) {
            begin(&after);
            CHECK(hf_create(&g_hf, HF_SETTINGS, bad[i], (uint32_t)strlen(bad[i])), "host copies a bad list %u", i);
            sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
            CHECK(strstr(st, "ERROR, the card list is wrong") != NULL && strstr(st, "Nothing will be applied") != NULL,
                  "status refuses it [%s]", st);
            e0 = host_flash_erases;
            usbs_end(&r);
            devcfg_load(&c);
            CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_CARDS && host_flash_erases == e0, "refused %u", i);
            CHECK(c.card_count == 4u && c.device_id == 654u && cards_verify(&c), "and nothing changed");
        }
        begin(&after);
        CHECK(hf_create(&g_hf, HF_SETTINGS, "#CARDS,2\r\n10\r\nten\r\n", 18u), "a bad line");
        sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
        CHECK(strstr(st, "at line 3") != NULL, "status names the line [%s]", st);
        usbs_end(&r);
    }

    /* A flash that fails while storing: refused, and no half list. */
    begin(&after);
    n = make_text(g_txt, sizeof(g_txt), other, 3u, 3u);
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "a good list");
    host_fail_writes_after(2u);
    usbs_end(&r);
    host_write_failures = 0u;
    devcfg_load(&c);
    CHECK(r.outcome == USBS_IMPORT_FAILED && r.rep.status == SETF_ERR_FLASH, "a flash failure is reported");
    CHECK(!c.valid || (c.card_count == 3u && cards_verify(&c)), "and leaves no half list");

    /* The largest list, the way the app sends it: 1000 cards and the usual lines. */
    host_flash_erase_all();
    devcfg_load(&c);
    for (i = 0u; i < NV_CARDS_MAX; i++) { many[i] = 4000000000u + (i * 9u); }
    begin(&c);
    n = (uint32_t)snprintf(g_txt, sizeof(g_txt), "#TIME,2026-09-10 13:00:00\r\n#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n#NEWSESSION,1\r\n#DEVICE,0000000321\r\n");
    n += make_text(&g_txt[n], sizeof(g_txt) - n, many, NV_CARDS_MAX, NV_CARDS_MAX);
    CHECK(n <= SETF_MAX_BYTES, "the whole file fits (%u of %u)", n, SETF_MAX_BYTES);
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "the host copies it");
    usbs_end(&r);
    devcfg_load(&c);
    CHECK(r.outcome == USBS_IMPORT_OK && c.card_count == NV_CARDS_MAX && cards_verify(&c) && c.device_id == 321u && r.session_start,
          "applied: list, ID and lecture together");

    /* The list as shown at that size still fits the window and scans clean. */
    begin(&c);
    n = shown();
    CHECK(n > 11000u && n <= SETF_MAX_BYTES, "shown back at %u bytes", n);
    {
        setf_report_t rep;

        scan(g_txt, &rep);
        CHECK(rep.status == SETF_OK && rep.card_count == NV_CARDS_MAX && rep.card_crc == c.card_crc, "and it parses to the same list");
    }
    e0 = host_flash_erases;
    CHECK(hf_create(&g_hf, HF_SETTINGS, g_txt, n), "saved untouched");
    usbs_end(&r);
    CHECK(!r.cards_set && host_flash_erases == e0, "no rewrite of an identical list");

    /* STATUS.TXT keeps its last line even in the worst case. */
    host_flash_erase_all();
    devcfg_load(&c);
    CHECK(store(&c, 4000000000u, four, 4u), "a device with a long ID and a list");
    begin(&c);
    {
        char bigtxt[400];
        uint32_t m = (uint32_t)snprintf(bigtxt, sizeof(bigtxt),
            "#TIME,2031-01-01 00:00:00\r\n#DEVICE,77\r\n#MODULE,%s\r\n#LECTURE,%s\r\n#NEWSESSION,1\r\n#TIME,garbage\r\n#CARDS,3\r\n1\r\n2\r\n3\r\n",
            "MMMMMMMMMMMMMMMMMMMMMMMM", "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL");

        CHECK(hf_create(&g_hf, HF_SETTINGS, bigtxt, m), "a file that changes everything");
        sn = hf_read(&g_hf, HF_STATUS, (uint8_t *)st, 512u); st[sn < 0 ? 0 : sn] = '\0';
        CHECK(strstr(st, "will start") != NULL && strstr(st, "ignored") != NULL,
              "the whole SETTINGS.CSV line fits in the status sector [%s]", st);
    }
    usbs_end(&r);
}

/* ===================================================================== */
/* The tap                                                                */
/* ===================================================================== */

static app_datetime_t at(uint32_t s)
{
    app_datetime_t dt = { 2026u, 9u, 10u, 13u, 0u, 0u };

    dt.hour = (uint8_t)(13u + (s / 3600u));
    dt.minute = (uint8_t)((s % 3600u) / 60u);
    dt.second = (uint8_t)(s % 60u);
    return dt;
}

static void pump(void)
{
    int i;

    for (i = 0; i < 40 && app_state() == ST_FEEDBACK; i++) { app_dispatch(APP_EVT_TIMER); }
}

static uint32_t tap(uint32_t id, uint32_t t)
{
    app_datetime_t dt = at(t);
    uint32_t fb;

    host_set_time(&dt);
    app_dispatch(APP_EVT_TOUCH);
    present_card(id);
    app_dispatch(APP_EVT_CAPTURE_FULL);
    fb = host_out_mask;
    pump();
    return fb;
}

static void test_cards_tap(void)
{
    static hostfs_t h;
    static char csv[4096];
    static char text[1024];
    static const uint32_t cards[] = { 1000u, 1007u, 2000u };
    uint32_t n, i, fb;
    int32_t rn;

    printf("cards: the tap\n");

    host_flash_erase_all();
    app_init();
    CHECK(tap(555u, 10u) == OK_FB, "no list yet: every card shows green");

    /* The host registers three cards and starts a lecture. */
    app_dispatch(APP_EVT_USB_ATTACH);
    hf_mount(&h);
    n = (uint32_t)snprintf(text, sizeof(text), "#MODULE,EN2090\r\n#LECTURE,Lecture 1\r\n");
    n += make_text(&text[n], sizeof(text) - n, cards, 3u, 3u);
    CHECK(hf_create(&h, HF_SETTINGS, text, n), "the app sends the list");
    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    fb = 0u;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_USB_DETACH);
        fb = host_out_mask;
        pump();
        CHECK(0, "should reach Standby");
    }
    host_deep_sleep_armed = false;
    CHECK((fb & PLAT_OUT_LED_GREEN) != 0u && (fb & PLAT_OUT_LED_RED) == 0u, "applied: green");

    app_init();
    CHECK(tap(1000u, 100u) == OK_FB, "a registered card: green and a buzz");
    CHECK(tap(2000u, 110u) == OK_FB, "another one");
    CHECK(tap(999u, 120u) == UNK_FB, "a stranger: red and a long buzz");
    CHECK(tap(1000u, 130u) == PLAT_OUT_VIBRATION, "a registered card again is still a duplicate");
    CHECK(tap(999u, 140u) == PLAT_OUT_VIBRATION, "so is a stranger again: recorded once per lecture");
    CHECK(tap(3000u, 150u) == UNK_FB, "a second stranger");

    /* Everything was recorded: the CSV is time and number, nothing else. */
    app_dispatch(APP_EVT_USB_ATTACH);
    hf_mount(&h);
    rn = hf_read(&h, HF_ATTEND, (uint8_t *)csv, sizeof(csv) - 1u);
    csv[rn < 0 ? 0 : rn] = '\0';
    CHECK(strncmp(csv, "DATE,TIME,CARD_ID ", 18u) == 0, "header");
    CHECK(strstr(csv, "2026-09-10,13:01:40,0000001000\r\n") != NULL, "known card logged");
    CHECK(strstr(csv, "2026-09-10,13:02:00,0000000999\r\n") != NULL, "unknown card logged too");
    CHECK(strstr(csv, "2026-09-10,13:02:30,0000003000\r\n") != NULL, "and the other");
    CHECK(strstr(csv, "0000000555") != NULL, "and the one from before the list existed");
    {
        uint32_t rows = 0u;

        for (i = 0u; csv[i] != '\0'; i++) { rows += (csv[i] == '\n') ? 1u : 0u; }
        CHECK(rows == 1u + 5u, "five rows and a header (%u lines)", rows);
    }
    CHECK(usbs_file_size() == 6u * CSV_ROW_BYTES, "file size");

    /* The list survives Standby and is checked again: damage turns it off. */
    host_deep_sleeps = 0u;
    host_deep_sleep_armed = true;
    if (setjmp(host_deep_sleep_jmp) == 0) {
        app_dispatch(APP_EVT_USB_DETACH);
        pump();
        CHECK(0, "should reach Standby");
    }
    host_deep_sleep_armed = false;
    app_init();
    CHECK(tap(5000u, 200u) == UNK_FB, "after a reboot the list is still in force");
    CHECK(tap(1007u, 210u) == OK_FB, "and still knows its cards");

    host_flash[NV_CARDS_OFFSET] ^= 0x10u;
    app_init();
    CHECK(tap(5001u, 300u) == OK_FB && tap(1007u, 310u) == OK_FB,
          "a list that fails its CRC is ignored: every card shows green");

}

void test_cards(void)
{
    test_cards_parser();
    test_cards_flash();
    test_cards_usb();
    test_cards_tap();
}
