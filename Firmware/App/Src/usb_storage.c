/**
 * @file    usb_storage.c
 * @brief   Level 2 (logic) — the exported volume.
 *
 * LBA routing:
 *   0                      boot sector (generated; writes ignored)
 *   1 .. 12, 13 .. 24      FAT 1 and FAT 2 (one RAM copy serves both)
 *   25                     root directory (RAM)
 *   26                     cluster 2: STATUS.TXT (generated, read-only)
 *   27 .. 54               clusters 3..30: SETTINGS.CSV window (RAM, read/write)
 *   55                     cluster 31: LASTCARD.TXT (generated, read-only)
 *   56 ..                  clusters 32..: ATTEND.CSV (generated, read-only)
 *   then                   LECTURES.CSV, the LECTURES directory (generated;
 *                          writes dropped) and the lecture files (generated,
 *                          read-only)
 *
 * Settings are applied in two steps so that a host can write the file in any
 * order it likes, over any number of clusters, and a half-copied or mistyped
 * file can never change the device: writes only land in RAM, and usbs_end()
 * validates the whole thing before it applies anything.
 */
#include "usb_storage.h"
#include "csv.h"
#include "device_cfg.h"
#include "platform_if.h"
#include "timeutil.h"

#define WIN_SECTORS   USBS_SETTINGS_CLUSTERS
#define WIN_LAST      (USBS_SETTINGS_CLUSTER + WIN_SECTORS - 1u)

/** Hosts differ on how much of the volume they will tolerate being tiny; keep
 *  the generated status file one full sector so its size never changes. */
#define STATUS_BYTES  FAT12_SECTOR_SIZE

static const char k_name_status[11]   = { 'S','T','A','T','U','S',' ',' ','T','X','T' };
static const char k_name_settings[11] = { 'S','E','T','T','I','N','G','S','C','S','V' };
static const char k_name_attend[11]   = { 'A','T','T','E','N','D',' ',' ','C','S','V' };
static const char k_name_lectures[11] = { 'L','E','C','T','U','R','E','S','C','S','V' };
static const char k_name_lastcard[11] = { 'L','A','S','T','C','A','R','D','T','X','T' };
static const char k_name_lectdir[11]  = { 'L','E','C','T','U','R','E','S',' ',' ',' ' };
static const char k_name_dot[11]      = { '.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
static const char k_name_dotdot[11]   = { '.','.',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
static const char k_label[11]         = { 'A','T','T','E','N','D','A','N','C','E',' ' };

/**
 * Where the session markers sit in the log. Markers are not attendance, so
 * ATTEND.CSV skips them; to turn a row number back into a log index the log is
 * scanned once at attach and the markers are kept here, in the order they were
 * written.
 */
typedef struct {
    uint16_t pos;           /**< Log index of the marker's header; bit 15 set for a stray text record. */
    uint16_t data_before;   /**< Attendance records before this marker. */
    uint16_t cum_after;     /**< Marker records up to and including this one. */
} usbs_mark_t;

#define MARK_STRAY   0x8000u    /**< A text record with no header: the rest of a torn marker. */
#define MARK_POS(m)  ((uint32_t)((m)->pos & 0x7FFFu))

/** Far more than a full log can hold at one marker per lecture. */
#define USBS_MAX_MARKS  768u

/**
 * One file in the LECTURES folder: the marker header it starts at (LF_NONE for
 * the taps before any lecture) and its first cluster. Its rows run to where
 * the next file's start, so nothing else needs storing.
 */
typedef struct {
    uint16_t mark;
    uint16_t cluster;
} usbs_lfile_t;

#define LF_NONE        0xFFFFu
/** "L001_2026-10-07_14-30.csv" */
#define LF_NAME_LEN    25u
/** Two long-name entries and the 8.3 entry. */
#define LF_ENTRIES     3u
/** "." and ".." */
#define LF_DOT_ENTRIES 2u

static const log_store_t *s_log;
static fat12_vol_t s_vol;
static bool s_active;
static bool s_data_read;

static uint32_t s_device_id;        /**< As shown at attach. */
static uint32_t s_records;          /**< Attendance rows, markers excluded. Latched at usbs_begin(). */
static uint32_t s_n_marks;
static uint32_t s_last_tap;         /**< Log index of the newest attendance record, or UINT32_MAX. */
static session_t s_cur;             /**< The newest session: what the file is shown with. */
static uint32_t s_attend_clusters;
static uint32_t s_attend_size;
static uint32_t s_n_lectures;       /**< Marker headers, i.e. LECTURES.CSV rows after its header. */
static uint32_t s_lect_cluster;     /**< First cluster of LECTURES.CSV, right after ATTEND.CSV. */
static uint32_t s_lect_clusters;
static uint32_t s_lect_size;
static uint16_t s_fat_date;
static uint16_t s_fat_time;

static uint32_t s_n_lfiles;         /**< Lecture files planned, s_lfiles[0 .. s_n_lfiles). */
static uint32_t s_lf_lo;            /**< First one shown; earlier ones did not fit. */
static uint32_t s_lf_number0;       /**< Lecture number of s_lfiles[0]: 0 when it is L000, else 1. */
static uint32_t s_dir_cluster;      /**< The LECTURES directory, after LECTURES.CSV. */
static uint32_t s_dir_clusters;
static uint32_t s_files_cluster;    /**< First cluster of the first file shown. */
static uint32_t s_end_cluster;      /**< One past the last cluster in use. */

/* LASTCARD.TXT. Written by the main loop inside a critical section, read by
 * the USB interrupt. */
static uint32_t s_card_taps;
static uint32_t s_card_id;
static uint8_t  s_card_uid[USBS_UID_MAX];
static uint8_t  s_card_uid_len;

/* STATUS.TXT's battery line, from the main loop's last good sample. */
static bool     s_batt_valid;
static uint32_t s_batt_mv;
static uint32_t s_batt_percent;

/* The part has a second 16 kB SRAM block that nothing else uses. The FAT, the
 * root directory and the marker table go there so the stack and the USB library
 * keep the main block to themselves. None of them needs zero-initialisation
 * (usbs_begin() fills each one), which is what lets the linker leave that
 * section NOLOAD. Elsewhere, such as the host tests, the attribute is simply
 * not applied. */
#if defined(__GNUC__) && defined(__arm__)
#define USBS_SRAM2  __attribute__((section(".ram2")))
#else
#define USBS_SRAM2
#endif

static uint8_t  s_fat[FAT12_FAT_BYTES] USBS_SRAM2;
static uint8_t  s_root[FAT12_SECTOR_SIZE] USBS_SRAM2;
static usbs_mark_t s_marks[USBS_MAX_MARKS] USBS_SRAM2;
static usbs_lfile_t s_lfiles[USBS_MAX_MARKS + 1u] USBS_SRAM2;
static uint8_t  s_win[WIN_SECTORS * FAT12_SECTOR_SIZE];
static uint64_t s_win_written;      /**< One bit per window sector the host wrote. */
static uint32_t s_settings_size;    /**< SETTINGS.CSV size as first shown. */
static device_cfg_t s_cfg;          /**< Device ID as shown at attach. */
static app_epoch_t s_shown_epoch;   /**< The clock as printed on the #TIME line. */

static uint8_t  s_map[WIN_SECTORS]; /**< File sector -> window sector. */
static uint32_t s_cur_size;
static bool     s_check_valid;      /**< s_check (STATUS.TXT) is up to date. */

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static void fill_zero(uint8_t *p, uint32_t n)
{
    while (n-- > 0u) {
        *p++ = 0u;
    }
}

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    while (n-- > 0u) {
        *dst++ = *src++;
    }
}

static bool names_equal(const uint8_t *d, const char *n11)
{
    uint8_t i;

    for (i = 0u; i < 11u; i++) {
        if (d[i] != (uint8_t)n11[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t clusters_for(uint32_t bytes)
{
    return (bytes + FAT12_SECTOR_SIZE - 1u) / FAT12_SECTOR_SIZE;
}

/* ------------------------------------------------------------------------ */
/* Session markers                                                          */
/* ------------------------------------------------------------------------ */

/**
 * The session in force at marker entry @p m: that entry, or the nearest real
 * header before it when @p m is a stray text record.
 * @return false when there is none.
 */
static bool session_entry(uint32_t m, uint32_t *out)
{
    uint32_t i = m;

    if (s_n_marks == 0u) {
        return false;
    }
    for (;;) {
        if ((s_marks[i].pos & MARK_STRAY) == 0u) {
            *out = i;
            return true;
        }
        if (i == 0u) {
            return false;
        }
        i--;
    }
}

/**
 * Walk the log once: find every marker, count the attendance records, note the
 * newest tap and remember the newest session. Reads each record once, a few
 * milliseconds at the 24 MHz the USB session runs at.
 */
static void scan_markers(const log_store_t *ls)
{
    uint32_t total = (ls != NULL) ? log_total(ls) : 0u;
    uint32_t data = 0u;
    uint32_t cum = 0u;
    uint32_t i = 0u;

    s_n_marks = 0u;
    s_n_lectures = 0u;
    s_last_tap = UINT32_MAX;
    s_cur.valid = false;
    s_cur.module[0] = '\0';
    s_cur.lecture[0] = '\0';

    while (i < total) {
        app_record_t r;

        if (!log_read(ls, i, &r)) {
            break;
        }
        if (sess_is_marker(r.student_id)) {
            uint32_t span = 1u;
            uint16_t flag = 0u;

            if (sess_is_header(r.student_id)) {
                /* Claim only the text records that are really there. A marker
                 * torn by a power cut has a header promising more than
                 * follows, and the records after it are attendance. */
                uint32_t want = sess_span(r.student_id) - 1u;
                app_record_t t;

                while (span <= want && (i + span) < total &&
                       log_read(ls, i + span, &t) && t.student_id == NV_MARK_TEXT) {
                    span++;
                }
            } else {
                flag = MARK_STRAY;      /* text with no header: the tail of a torn marker */
            }

            if (s_n_marks >= USBS_MAX_MARKS) {
                break;          /* table full (see Known limitations in ARCHITECTURE.md) */
            }
            cum += span;
            if (flag == 0u) {
                s_n_lectures++;
            }
            s_marks[s_n_marks].pos = (uint16_t)(i | flag);
            s_marks[s_n_marks].data_before = (uint16_t)data;
            s_marks[s_n_marks].cum_after = (uint16_t)cum;
            s_n_marks++;
            i += span;
        } else {
            data++;
            s_last_tap = i;
            i++;
        }
    }
    s_records = data;

    {
        uint32_t m;

        if (session_entry(s_n_marks - 1u, &m) && s_n_marks > 0u) {
            (void)sess_read(ls, MARK_POS(&s_marks[m]), &s_cur);
        }
    }
}

/**
 * The marker entry in force at an attendance row: the last one written before
 * it. Returns false for a row that precedes every marker. The log index of
 * attendance row @p row is then row + marker records before it.
 */
static bool find_mark(uint32_t row, uint32_t *mark)
{
    uint32_t lo = 0u;
    uint32_t hi = s_n_marks;

    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2u);

        if (s_marks[mid].data_before <= row) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == 0u) {
        return false;
    }
    *mark = lo - 1u;
    return true;
}

/** True when the host's #MODULE / #LECTURE / #NEWSESSION start a new session. */
static bool session_would_start(const setf_report_t *r)
{
    uint32_t i;

    if (r->new_session) {
        return true;
    }
    if (r->has_module) {
        for (i = 0u; i <= SESS_MODULE_MAX; i++) {
            if (r->module[i] != s_cur.module[i]) {
                return true;
            }
            if (r->module[i] == '\0') {
                break;
            }
        }
    }
    if (r->has_lecture) {
        for (i = 0u; i <= SESS_LECTURE_MAX; i++) {
            if (r->lecture[i] != s_cur.lecture[i]) {
                return true;
            }
            if (r->lecture[i] == '\0') {
                break;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* The LECTURES folder                                                      */
/* ------------------------------------------------------------------------ */

/** Attendance rows before lecture file @p f begins. */
static uint32_t lfile_begin(uint32_t f)
{
    uint32_t b = (s_lfiles[f].mark == LF_NONE) ? 0u : s_marks[s_lfiles[f].mark].data_before;

    return (b > s_records) ? s_records : b;
}

/** Attendance rows in lecture file @p f: up to where the next one begins. */
static uint32_t lfile_rows(uint32_t f)
{
    uint32_t e = ((f + 1u) < s_n_lfiles) ? lfile_begin(f + 1u) : s_records;
    uint32_t b = lfile_begin(f);

    return (e > b) ? (e - b) : 0u;
}

static uint32_t lfile_clusters(uint32_t f)
{
    return clusters_for(csv_size(lfile_rows(f)));
}

static uint32_t dir_clusters_for(uint32_t files)
{
    uint32_t n = clusters_for((LF_DOT_ENTRIES + (files * LF_ENTRIES)) * 32u);

    return (n == 0u) ? 1u : n;
}

/**
 * One file per lecture marker, plus L000 for taps logged before the first
 * one. They go after everything else and get whatever clusters are left; if
 * that is not enough (it is, with the shipped log size) the oldest are left
 * out rather than the newest.
 */
static void plan_lecture_files(uint32_t first_cluster, uint32_t budget)
{
    uint32_t m, f, need;
    bool before;
    bool any_header = false;
    uint32_t first_header = 0u;

    s_n_lfiles = 0u;
    for (m = 0u; m < s_n_marks; m++) {
        if ((s_marks[m].pos & MARK_STRAY) == 0u) {
            if (!any_header) {
                any_header = true;
                first_header = m;
            }
        }
    }
    before = any_header ? (s_marks[first_header].data_before > 0u) : (s_records > 0u);
    s_lf_number0 = before ? 0u : 1u;
    if (before) {
        s_lfiles[s_n_lfiles].mark = LF_NONE;
        s_lfiles[s_n_lfiles].cluster = 0u;
        s_n_lfiles++;
    }
    for (m = 0u; m < s_n_marks; m++) {
        if ((s_marks[m].pos & MARK_STRAY) == 0u) {
            s_lfiles[s_n_lfiles].mark = (uint16_t)m;
            s_lfiles[s_n_lfiles].cluster = 0u;
            s_n_lfiles++;
        }
    }

    need = 0u;
    for (f = 0u; f < s_n_lfiles; f++) {
        need += lfile_clusters(f);
    }
    s_lf_lo = 0u;
    while (s_lf_lo < s_n_lfiles &&
           (need + dir_clusters_for(s_n_lfiles - s_lf_lo)) > budget) {
        need -= lfile_clusters(s_lf_lo);
        s_lf_lo++;
    }

    s_dir_cluster = first_cluster;
    s_dir_clusters = dir_clusters_for(s_n_lfiles - s_lf_lo);
    if (s_dir_clusters > budget) {
        s_dir_clusters = 0u;    /* no room even for the folder: leave it out */
    }
    s_files_cluster = s_dir_cluster + s_dir_clusters;
    s_end_cluster = s_files_cluster;
    for (f = s_lf_lo; f < s_n_lfiles; f++) {
        s_lfiles[f].cluster = (uint16_t)s_end_cluster;
        s_end_cluster += lfile_clusters(f);
    }
}

uint32_t usbs_lecture_file_count(void)
{
    return (s_dir_clusters > 0u) ? (s_n_lfiles - s_lf_lo) : 0u;
}

/* ------------------------------------------------------------------------ */
/* Session start                                                            */
/* ------------------------------------------------------------------------ */

void usbs_begin(const log_store_t *ls, const device_cfg_t *cfg, const app_datetime_t *now)
{
    const uint32_t device_id = cfg->device_id;
    /* Clusters every volume has: STATUS.TXT, the window, LASTCARD.TXT. */
    const uint32_t fixed = 1u + WIN_SECTORS + 1u;

    uint32_t max_attend_clusters;
    uint32_t total_clusters;
    uint32_t size;
    uint32_t f;

    s_log = ls;
    s_cfg = *cfg;
    s_device_id = device_id;
    s_data_read = false;
    s_active = true;
    s_win_written = 0u;
    s_check_valid = false;
    s_card_taps = 0u;
    s_card_id = 0u;
    s_card_uid_len = 0u;
    scan_markers(ls);       /* sets s_records, the rows ATTEND.CSV will have */

    /* LECTURES.CSV is at most 769 rows of 128 bytes (193 clusters); it is
     * sized first so ATTEND.CSV's clamp below leaves room for it. */
    s_lect_size = csv_lecture_size(s_n_lectures);
    s_lect_clusters = clusters_for(s_lect_size);
    max_attend_clusters = FAT12_MAX_CLUSTERS - fixed - s_lect_clusters;

    size = csv_size(s_records);
    s_attend_clusters = clusters_for(size);
    if (s_attend_clusters > max_attend_clusters) {
        /* Clamp rather than expose a file the FAT chain cannot describe.
         * The log area is smaller than the volume, so this cannot happen with
         * the shipped geometry; it guards a future capacity change. */
        s_attend_clusters = max_attend_clusters;
        s_records = ((s_attend_clusters * FAT12_SECTOR_SIZE) / CSV_ROW_BYTES) - 1u;
        size = csv_size(s_records);
    }
    s_attend_size = size;
    s_lect_cluster = USBS_ATTEND_CLUSTER + s_attend_clusters;

    plan_lecture_files(s_lect_cluster + s_lect_clusters,
                       FAT12_MAX_CLUSTERS - fixed - s_attend_clusters - s_lect_clusters);

    total_clusters = s_end_cluster - FAT12_FIRST_CLUSTER;
    s_vol.total_sectors = FAT12_DATA_START_LBA + total_clusters;
    s_vol.volume_serial = (device_id != 0u) ? device_id : 0x43415331u;
    fat12_pack_datetime(now, &s_fat_date, &s_fat_time);
    s_shown_epoch = time_to_epoch(now);

    /* SETTINGS.CSV: the current settings as text, in a zeroed window. */
    fill_zero(s_win, sizeof(s_win));
    s_settings_size = setf_render((char *)s_win, sizeof(s_win), now,
                                  s_cur.module, s_cur.lecture, device_id);
    if (s_settings_size == 0u) {
        /* Cannot happen with names this short, but a window the file does not
         * fit would be worse than an empty one the host can still replace. */
        fill_zero(s_win, sizeof(s_win));
    }

    /* FAT and root directory, in RAM so the host's own changes stick. */
    fat12_fat_init(s_fat);
    fat12_chain(s_fat, USBS_STATUS_CLUSTER, 1u);
    if (s_settings_size > 0u) {
        fat12_chain(s_fat, USBS_SETTINGS_CLUSTER, clusters_for(s_settings_size));
    }
    fat12_chain(s_fat, USBS_LASTCARD_CLUSTER, 1u);
    fat12_chain(s_fat, USBS_ATTEND_CLUSTER, s_attend_clusters);
    fat12_chain(s_fat, s_lect_cluster, s_lect_clusters);
    if (s_dir_clusters > 0u) {
        fat12_chain(s_fat, s_dir_cluster, s_dir_clusters);
        for (f = s_lf_lo; f < s_n_lfiles; f++) {
            fat12_chain(s_fat, s_lfiles[f].cluster, lfile_clusters(f));
        }
    }

    fill_zero(s_root, sizeof(s_root));
    fat12_dirent(&s_root[0], k_label, FAT12_ATTR_VOLUME_ID, 0u, 0u, s_fat_date, s_fat_time);
    fat12_dirent(&s_root[32], k_name_status, FAT12_ATTR_READ_ONLY | FAT12_ATTR_ARCHIVE,
                 (uint16_t)USBS_STATUS_CLUSTER, STATUS_BYTES, s_fat_date, s_fat_time);
    fat12_dirent(&s_root[64], k_name_settings, FAT12_ATTR_ARCHIVE,
                 (s_settings_size > 0u) ? (uint16_t)USBS_SETTINGS_CLUSTER : 0u,
                 s_settings_size, s_fat_date, s_fat_time);
    fat12_dirent(&s_root[96], k_name_attend, FAT12_ATTR_READ_ONLY | FAT12_ATTR_ARCHIVE,
                 (uint16_t)USBS_ATTEND_CLUSTER, s_attend_size, s_fat_date, s_fat_time);
    fat12_dirent(&s_root[128], k_name_lectures, FAT12_ATTR_READ_ONLY | FAT12_ATTR_ARCHIVE,
                 (uint16_t)s_lect_cluster, s_lect_size, s_fat_date, s_fat_time);
    fat12_dirent(&s_root[160], k_name_lastcard, FAT12_ATTR_READ_ONLY | FAT12_ATTR_ARCHIVE,
                 (uint16_t)USBS_LASTCARD_CLUSTER, STATUS_BYTES, s_fat_date, s_fat_time);
    if (s_dir_clusters > 0u) {
        fat12_dirent(&s_root[192], k_name_lectdir, FAT12_ATTR_DIRECTORY,
                     (uint16_t)s_dir_cluster, 0u, s_fat_date, s_fat_time);
    }
}

void usbs_set_battery(uint32_t mv, uint8_t percent)
{
    plat_critical_enter();
    s_batt_mv = mv;
    s_batt_percent = percent;
    s_batt_valid = (mv != 0u);
    plat_critical_exit();
}

uint32_t usbs_sector_count(void)
{
    return s_vol.total_sectors;
}

uint16_t usbs_sector_size(void)
{
    return FAT12_SECTOR_SIZE;
}

uint32_t usbs_file_size(void)
{
    return s_attend_size;
}

uint32_t usbs_settings_size(void)
{
    return s_settings_size;
}

bool usbs_file_was_read(void)
{
    return s_data_read;
}

/* ------------------------------------------------------------------------ */
/* Finding and checking the host's SETTINGS.CSV                             */
/* ------------------------------------------------------------------------ */

typedef enum {
    A_NONE,        /**< The file is not there (the host deleted it). */
    A_BADFILE,     /**< A file whose clusters make no sense. */
    A_SCANNED      /**< Parsed; see @c rep. */
} analysis_kind_t;

typedef struct {
    analysis_kind_t kind;
    bool            changed;   /**< Differs from what was shown at attach. */
    setf_report_t   rep;
} analysis_t;

static bool entry_is_file(const uint8_t *d)
{
    return (d[0] != 0x00u) && (d[0] != 0xE5u) &&
           ((d[11] & (FAT12_ATTR_VOLUME_ID | FAT12_ATTR_DIRECTORY)) == 0u);
}

static const uint8_t *find_settings(void)
{
    uint32_t i;

    for (i = 0u; i < FAT12_ROOT_ENTRIES; i++) {
        const uint8_t *d = &s_root[i * 32u];

        if (d[0] == 0x00u) {
            break;
        }
        if (entry_is_file(d) && names_equal(d, k_name_settings)) {
            return d;
        }
    }
    return NULL;
}

/**
 * Follow @p d's cluster chain through the window into s_map.
 *
 * @param touched  Set when the host wrote any sector the file uses.
 * @return SETF_OK, or the reason the file cannot be read.
 */
static setf_status_t map_file(const uint8_t *d, uint32_t *size, bool *touched)
{
    uint32_t first = fat12_rd16(&d[26]);
    uint32_t sz = fat12_rd32(&d[28]);
    uint32_t n, k, c;
    uint64_t visited = 0u;

    *touched = false;
    *size = sz;

    if (sz == 0u) {
        return SETF_ERR_EMPTY;
    }
    if (sz > SETF_MAX_BYTES) {
        return SETF_ERR_TOO_LARGE;
    }

    n = clusters_for(sz);
    c = first;
    for (k = 0u; k < n; k++) {
        uint32_t w;

        if (c < USBS_SETTINGS_CLUSTER || c > WIN_LAST) {
            return SETF_ERR_FILE;       /* free, end-of-chain or outside the window */
        }
        w = c - USBS_SETTINGS_CLUSTER;
        if ((visited >> w) & 1u) {
            return SETF_ERR_FILE;       /* the chain loops */
        }
        visited |= (uint64_t)1u << w;
        if ((s_win_written >> w) & 1u) {
            *touched = true;
        }
        s_map[k] = (uint8_t)w;
        if ((k + 1u) < n) {
            c = fat12_get(s_fat, c);
        }
    }
    return SETF_OK;
}

static int get_byte(void *ctx, uint32_t offset)
{
    (void)ctx;
    if (offset >= s_cur_size) {
        return -1;
    }
    return s_win[((uint32_t)s_map[offset / FAT12_SECTOR_SIZE] * FAT12_SECTOR_SIZE)
                 + (offset % FAT12_SECTOR_SIZE)];
}

static void report_clear(setf_report_t *r, setf_status_t status)
{
    r->status = status;
    r->has_time = false;
    r->has_device = false;
    r->device_id = 0u;
    r->bad_directive = false;
    r->has_module = false;
    r->has_lecture = false;
    r->new_session = false;
    r->clear_log = false;
    r->module[0] = '\0';
    r->lecture[0] = '\0';
}

/** Find the host's SETTINGS.CSV, map it, and parse it. */
static void analyse(analysis_t *a)
{
    const uint8_t *file = find_settings();
    bool touched = false;
    setf_status_t st;
    uint32_t size = 0u;

    a->kind = A_NONE;
    a->changed = false;
    report_clear(&a->rep, SETF_OK);

    if (file == NULL) {
        return;
    }

    st = map_file(file, &size, &touched);
    a->changed = (st != SETF_OK) || touched ||
                 (fat12_rd16(&file[26]) != USBS_SETTINGS_CLUSTER) ||
                 (size != s_settings_size);
    if (st != SETF_OK) {
        a->kind = A_BADFILE;
        report_clear(&a->rep, st);
        return;
    }

    s_cur_size = size;
    setf_scan(get_byte, NULL, size, &a->rep);
    a->kind = A_SCANNED;
}

/* ------------------------------------------------------------------------ */
/* STATUS.TXT                                                               */
/* ------------------------------------------------------------------------ */

/** Last analysis shown by STATUS.TXT; s_check_valid is cleared by every host write. */
static analysis_t s_check;

typedef struct {
    uint8_t *buf;
    uint32_t len;
} sb_t;

static void sb_char(sb_t *s, char c)
{
    /* Leave room for the closing CRLF. */
    if (s->len < (STATUS_BYTES - 2u)) {
        s->buf[s->len++] = (uint8_t)c;
    }
}

static void sb_str(sb_t *s, const char *t)
{
    while (*t != '\0') {
        sb_char(s, *t++);
    }
}

static void sb_dec(sb_t *s, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0u;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);
    while (n > 0u) {
        sb_char(s, tmp[--n]);
    }
}

static void sb_pad(sb_t *s, uint32_t v, uint8_t width)
{
    char tmp[10];
    uint8_t i;

    csv_put_padded(tmp, v, width);
    for (i = 0u; i < width; i++) {
        sb_char(s, tmp[i]);
    }
}

static void sb_eol(sb_t *s)
{
    sb_str(s, "\r\n");
}

/** Pad to the end of the sector so the file is a clean 512 text bytes. */
static void sb_finish(sb_t *s)
{
    uint32_t i;

    for (i = s->len; i < (STATUS_BYTES - 2u); i++) {
        s->buf[i] = (uint8_t)' ';
    }
    s->buf[STATUS_BYTES - 2u] = (uint8_t)'\r';
    s->buf[STATUS_BYTES - 1u] = (uint8_t)'\n';
}

static void sb_datetime(sb_t *s, const app_datetime_t *d)
{
    sb_pad(s, d->year, 4u); sb_char(s, '-'); sb_pad(s, d->month, 2u);
    sb_char(s, '-'); sb_pad(s, d->day, 2u); sb_char(s, ' ');
    sb_pad(s, d->hour, 2u); sb_char(s, ':'); sb_pad(s, d->minute, 2u);
    sb_char(s, ':'); sb_pad(s, d->second, 2u);
}

static const char *error_text(const setf_report_t *r)
{
    switch (r->status) {
    case SETF_ERR_EMPTY:     return "ERROR, the file is empty";
    case SETF_ERR_TOO_LARGE: return "ERROR, the file is too large";
    case SETF_ERR_FILE:      return "ERROR, the file could not be read, copy it again";
    case SETF_ERR_FLASH:     return "ERROR, the device could not store the setting";
    default:                 return "ERROR";
    }
}

static void status_sector(uint8_t *out)
{
    sb_t s;
    app_datetime_t now;

    /* Parsing the host's file is quick, but this runs in the USB interrupt, so
     * redo it only after the host has written something since the last look. */
    if (!s_check_valid) {
        analyse(&s_check);
        s_check_valid = true;
    }
    const analysis_t a = s_check;

    s.buf = out;
    s.len = 0u;
    fill_zero(out, FAT12_SECTOR_SIZE);
    plat_rtc_get(&now);

    sb_str(&s, "ATTENDANCE LOGGER"); sb_eol(&s);
    if (s_device_id != 0u) {
        sb_str(&s, "Device ID    : "); sb_pad(&s, s_device_id, 10u); sb_eol(&s);
    }
    sb_str(&s, "Clock        : "); sb_datetime(&s, &now); sb_eol(&s);
    sb_str(&s, "Battery      : ");
    if (s_batt_valid) {
        sb_dec(&s, s_batt_percent); sb_str(&s, " % ("); sb_dec(&s, s_batt_mv); sb_str(&s, " mV)");
    } else {
        sb_str(&s, "unknown");
    }
    sb_eol(&s);
    sb_str(&s, "Attendance   : "); sb_dec(&s, s_records); sb_str(&s, " records in ATTEND.CSV");
    sb_eol(&s);
    sb_str(&s, "Lectures     : "); sb_dec(&s, usbs_lecture_file_count());
    sb_str(&s, " files in the LECTURES folder"); sb_eol(&s);

    sb_str(&s, "Last tap     : ");
    {
        app_record_t r;

        if (s_last_tap != UINT32_MAX && s_log != NULL && log_read(s_log, s_last_tap, &r)) {
            app_datetime_t t;

            time_from_epoch(r.stamp, &t);
            sb_pad(&s, r.student_id, 10u); sb_str(&s, " at "); sb_datetime(&s, &t);
        } else {
            sb_str(&s, "none yet");
        }
    }
    sb_eol(&s);

    sb_str(&s, "Lecture      : ");
    if (s_cur.valid) {
        app_datetime_t since;

        /* "since" is the device clock when the lecture started: the PC uses it
         * to decide which taps belong to which lecture, whatever its own clock says. */
        time_from_epoch(s_cur.start, &since);
        sb_str(&s, s_cur.module); sb_str(&s, " / "); sb_str(&s, s_cur.lecture);
        sb_str(&s, " (since "); sb_datetime(&s, &since); sb_char(&s, ')');
    } else {
        sb_str(&s, "none set");
    }
    sb_eol(&s);

    sb_str(&s, "SETTINGS.CSV : ");
    if (a.kind == A_NONE) {
        sb_str(&s, "not found, the device keeps its settings");
    } else if (a.kind == A_SCANNED && a.rep.status == SETF_OK) {
        if (!a.changed) {
            sb_str(&s, "unchanged");
        } else {
            bool any = false;

            sb_str(&s, "OK, will be applied when you eject the drive, press the button or unplug");
            if (a.rep.has_time) {
                sb_str(&s, "; clock will be set"); any = true;
            }
            if (a.rep.has_device && a.rep.device_id != s_device_id) {
                sb_str(&s, "; device ID will change"); any = true;
            }
            if (a.rep.clear_log) {
                sb_str(&s, "; all records will be deleted"); any = true;
            }
            if (session_would_start(&a.rep)) {
                sb_str(&s, "; a new lecture will start"); any = true;
            }
            if (!any) {
                sb_str(&s, "; nothing to change");
            }
            if (a.rep.bad_directive) {
                sb_str(&s, "; a #TIME or #DEVICE value was ignored");
            }
        }
    } else {
        sb_str(&s, error_text(&a.rep));
        sb_str(&s, ". Nothing will be applied");
    }
    sb_eol(&s);
    sb_finish(&s);
}

/* ------------------------------------------------------------------------ */
/* LASTCARD.TXT                                                             */
/* ------------------------------------------------------------------------ */

void usbs_set_last_card(uint32_t id, const uint8_t *uid, uint8_t uid_len)
{
    uint8_t i;
    uint8_t n = (uid_len > USBS_UID_MAX) ? (uint8_t)USBS_UID_MAX : uid_len;

    plat_critical_enter();
    s_card_id = id;
    for (i = 0u; i < n; i++) {
        s_card_uid[i] = uid[i];
    }
    s_card_uid_len = n;
    s_card_taps++;
    plat_critical_exit();
}

uint32_t usbs_last_card_taps(void)
{
    return s_card_taps;
}

/**
 * The card read last, for the PC app's "tap to register". Taps counts every
 * card read this session, so the app notices the same card tapped twice. The
 * host caches files, so the app reads this one past its cache.
 */
static void lastcard_sector(uint8_t *out)
{
    sb_t s;
    uint8_t i;

    s.buf = out;
    s.len = 0u;
    fill_zero(out, FAT12_SECTOR_SIZE);

    sb_str(&s, "LAST CARD"); sb_eol(&s);
    sb_str(&s, "Taps    : "); sb_dec(&s, s_card_taps); sb_eol(&s);
    sb_str(&s, "Card ID : ");
    if (s_card_taps == 0u) {
        sb_str(&s, "none yet");
    } else {
        sb_pad(&s, s_card_id, 10u);
    }
    sb_eol(&s);
    sb_str(&s, "UID     : ");
    if (s_card_taps == 0u || s_card_uid_len == 0u) {
        sb_char(&s, '-');
    } else {
        for (i = 0u; i < s_card_uid_len; i++) {
            if (i > 0u) {
                sb_char(&s, ' ');
            }
            sb_char(&s, "0123456789ABCDEF"[s_card_uid[i] >> 4]);
            sb_char(&s, "0123456789ABCDEF"[s_card_uid[i] & 0x0Fu]);
        }
    }
    sb_eol(&s);
    sb_finish(&s);
}

/* ------------------------------------------------------------------------ */
/* Read path                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Render one sector of ATTEND.CSV.
 *
 * CSV_ROW_BYTES divides FAT12_SECTOR_SIZE exactly, so a sector is always a
 * whole number of rows and the first row index is a plain multiply. Row 0 of
 * the file is the CSV header; row n+1 is attendance record n.
 */
/** Attendance row @p row_no (markers excluded) from the log. */
static bool attend_record(uint32_t row_no, app_record_t *rec)
{
    uint32_t mark = 0u;
    bool has_entry = find_mark(row_no, &mark);
    uint32_t record_index = has_entry ? (row_no + s_marks[mark].cum_after) : row_no;

    return row_no < s_records && s_log != NULL && log_read(s_log, record_index, rec);
}

/**
 * Render one sector of a CSV of attendance rows @p first .. @p first + @p rows:
 * the header, then one row per record. ATTEND.CSV is all of them, a lecture
 * file its own stretch.
 */
static void attend_sector(uint32_t first, uint32_t rows, uint32_t file_sector, uint8_t *buf)
{
    uint32_t first_row = file_sector * CSV_ROWS_PER_SECTOR;
    uint32_t i;

    for (i = 0u; i < CSV_ROWS_PER_SECTOR; i++) {
        char *row = (char *)&buf[i * CSV_ROW_BYTES];
        uint32_t global = first_row + i;
        app_record_t rec;

        if (global == 0u) {
            csv_header(row);
        } else if ((global - 1u) < rows && attend_record(first + global - 1u, &rec)) {
            csv_row(&rec, row);
        } else {
            /* Past end of file. The host should not be looking here, but a
             * read-ahead will, so return zeros rather than stale data. */
            fill_zero((uint8_t *)row, CSV_ROW_BYTES);
        }
    }
}

/**
 * Render one sector of ATTEND.CSV.
 *
 * CSV_ROW_BYTES divides FAT12_SECTOR_SIZE exactly, so a sector is always a
 * whole number of rows and the first row index is a plain multiply. Row 0 of
 * the file is the CSV header; row n+1 is attendance record n.
 */
static void read_attend_sector(uint32_t file_sector, uint8_t *buf)
{
    attend_sector(0u, s_records, file_sector, buf);
}

/* ---- The LECTURES folder ---- */

/** When lecture file @p f starts: its marker, or for L000 its first tap. */
static app_epoch_t lfile_stamp(uint32_t f)
{
    app_record_t r;

    if (s_lfiles[f].mark != LF_NONE) {
        if (s_log != NULL && log_read(s_log, MARK_POS(&s_marks[s_lfiles[f].mark]), &r)) {
            return r.stamp;
        }
    } else if (attend_record(0u, &r)) {
        return r.stamp;
    }
    return 0u;
}

/** "L001_2026-10-07_14-30.csv" and its 8.3 alias "L001    CSV". */
static void lfile_names(uint32_t f, char lfn[LF_NAME_LEN], char sfn[11])
{
    static const char k_alias_pad[7] = { ' ',' ',' ',' ','C','S','V' };
    uint32_t number = s_lf_number0 + f;
    app_datetime_t t;
    uint8_t i;

    time_from_epoch(lfile_stamp(f), &t);
    lfn[0] = 'L';
    csv_put_padded(&lfn[1], number, 3u);
    lfn[4] = '_';
    csv_put_padded(&lfn[5], t.year, 4u);
    lfn[9] = '-';
    csv_put_padded(&lfn[10], t.month, 2u);
    lfn[12] = '-';
    csv_put_padded(&lfn[13], t.day, 2u);
    lfn[15] = '_';
    csv_put_padded(&lfn[16], t.hour, 2u);
    lfn[18] = '-';
    csv_put_padded(&lfn[19], t.minute, 2u);
    lfn[21] = '.';
    lfn[22] = 'c';
    lfn[23] = 's';
    lfn[24] = 'v';

    for (i = 0u; i < 4u; i++) {
        sfn[i] = lfn[i];
    }
    for (i = 0u; i < 7u; i++) {
        sfn[4u + i] = k_alias_pad[i];
    }
}

/**
 * One sector of the LECTURES directory: ".", "..", then for every file two
 * long-name entries and its 8.3 entry, dated when the lecture started.
 */
static void read_dir_sector(uint32_t dir_sector, uint8_t *buf)
{
    const uint32_t per_sector = FAT12_SECTOR_SIZE / 32u;
    const uint32_t shown = s_n_lfiles - s_lf_lo;
    uint32_t i;

    fill_zero(buf, FAT12_SECTOR_SIZE);
    for (i = 0u; i < per_sector; i++) {
        uint8_t *d = &buf[i * 32u];
        uint32_t e = (dir_sector * per_sector) + i;

        if (e == 0u) {
            fat12_dirent(d, k_name_dot, FAT12_ATTR_DIRECTORY, (uint16_t)s_dir_cluster, 0u,
                         s_fat_date, s_fat_time);
        } else if (e == 1u) {
            fat12_dirent(d, k_name_dotdot, FAT12_ATTR_DIRECTORY, 0u, 0u, s_fat_date, s_fat_time);
        } else if (((e - LF_DOT_ENTRIES) / LF_ENTRIES) < shown) {
            uint32_t f = s_lf_lo + ((e - LF_DOT_ENTRIES) / LF_ENTRIES);
            uint32_t part = (e - LF_DOT_ENTRIES) % LF_ENTRIES;
            char lfn[LF_NAME_LEN];
            char sfn[11];

            lfile_names(f, lfn, sfn);
            if (part < (LF_ENTRIES - 1u)) {
                /* Highest long-name piece first. */
                fat12_lfn_entry(d, (uint8_t)((LF_ENTRIES - 1u) - part), fat12_sfn_checksum(sfn),
                                lfn, LF_NAME_LEN);
            } else {
                app_datetime_t t;
                uint16_t date, time;

                time_from_epoch(lfile_stamp(f), &t);
                fat12_pack_datetime(&t, &date, &time);
                fat12_dirent(d, sfn, FAT12_ATTR_READ_ONLY | FAT12_ATTR_ARCHIVE,
                             s_lfiles[f].cluster, csv_size(lfile_rows(f)), date, time);
            }
        } else {
            break;      /* zeros: the end of the directory */
        }
    }
}

/** One sector of a lecture file: find which file holds @p cluster. */
static void read_lfile_sector(uint32_t cluster, uint8_t *buf)
{
    uint32_t lo = s_lf_lo;
    uint32_t hi = s_n_lfiles;

    /* The last file starting at or before the cluster. */
    while ((hi - lo) > 1u) {
        uint32_t mid = lo + ((hi - lo) / 2u);

        if (s_lfiles[mid].cluster <= cluster) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    attend_sector(lfile_begin(lo), lfile_rows(lo), cluster - s_lfiles[lo].cluster, buf);
}

/**
 * Render one sector of LECTURES.CSV: row 0 the header, row n+1 the n-th
 * marker header in the log (stray text records are not lectures). A torn
 * marker shows the names it still has.
 */
static void read_lectures_sector(uint32_t file_sector, uint8_t *buf)
{
    uint32_t i;

    for (i = 0u; i < CSV_LECTURE_ROWS_PER_SECTOR; i++) {
        char *row = (char *)&buf[i * CSV_LECTURE_ROW_BYTES];
        uint32_t global = (file_sector * CSV_LECTURE_ROWS_PER_SECTOR) + i;
        uint32_t want, m;
        session_t sess;

        if (global == 0u) {
            csv_lecture_header(row);
            continue;
        }
        fill_zero((uint8_t *)row, CSV_LECTURE_ROW_BYTES);
        if ((global - 1u) >= s_n_lectures || s_log == NULL) {
            continue;       /* past the end: zeros, as for ATTEND.CSV */
        }
        want = global - 1u;
        for (m = 0u; m < s_n_marks; m++) {
            if ((s_marks[m].pos & MARK_STRAY) != 0u) {
                continue;
            }
            if (want == 0u) {
                break;
            }
            want--;
        }
        if (m < s_n_marks && sess_read(s_log, MARK_POS(&s_marks[m]), &sess)) {
            csv_lecture_row(sess.start, sess.module, sess.lecture, row);
        }
    }
}

static bool read_one(uint32_t lba, uint8_t *buf)
{
    uint32_t cluster;

    if (lba >= s_vol.total_sectors) {
        return false;
    }

    if (lba < FAT12_FAT_START_LBA) {
        fat12_boot_sector(&s_vol, buf);
        return true;
    }

    if (lba < FAT12_ROOT_START_LBA) {
        /* Both FAT copies are one RAM table. */
        uint32_t within = (lba - FAT12_FAT_START_LBA) % FAT12_SECTORS_PER_FAT;

        copy(buf, &s_fat[within * FAT12_SECTOR_SIZE], FAT12_SECTOR_SIZE);
        return true;
    }

    if (lba < FAT12_DATA_START_LBA) {
        copy(buf, &s_root[(lba - FAT12_ROOT_START_LBA) * FAT12_SECTOR_SIZE],
             FAT12_SECTOR_SIZE);
        return true;
    }

    cluster = FAT12_FIRST_CLUSTER + (lba - FAT12_DATA_START_LBA);
    if (cluster == USBS_STATUS_CLUSTER) {
        status_sector(buf);
    } else if (cluster <= WIN_LAST) {
        copy(buf, &s_win[(cluster - USBS_SETTINGS_CLUSTER) * FAT12_SECTOR_SIZE],
             FAT12_SECTOR_SIZE);
    } else if (cluster == USBS_LASTCARD_CLUSTER) {
        lastcard_sector(buf);
    } else if (cluster < s_lect_cluster) {
        s_data_read = true;
        read_attend_sector(cluster - USBS_ATTEND_CLUSTER, buf);
    } else if (cluster < s_dir_cluster) {
        read_lectures_sector(cluster - s_lect_cluster, buf);
    } else if (cluster < s_files_cluster) {
        read_dir_sector(cluster - s_dir_cluster, buf);
    } else {
        read_lfile_sector(cluster, buf);
    }
    return true;
}

bool usbs_read(uint32_t lba, uint8_t *buf, uint32_t count)
{
    uint32_t i;

    if ((lba + count) > s_vol.total_sectors) {
        return false;
    }

    for (i = 0u; i < count; i++) {
        if (!read_one(lba + i, &buf[i * FAT12_SECTOR_SIZE])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Write path                                                               */
/* ------------------------------------------------------------------------ */

static bool write_one(uint32_t lba, const uint8_t *buf)
{
    uint32_t cluster;

    if (lba >= s_vol.total_sectors) {
        return false;
    }
    s_check_valid = false;      /* the host is changing what STATUS.TXT reports on */

    if (lba < FAT12_FAT_START_LBA) {
        return true;    /* accepted and ignored: nothing in it is ours to change */
    }

    if (lba < FAT12_ROOT_START_LBA) {
        uint32_t within = (lba - FAT12_FAT_START_LBA) % FAT12_SECTORS_PER_FAT;

        copy(&s_fat[within * FAT12_SECTOR_SIZE], buf, FAT12_SECTOR_SIZE);
        return true;
    }

    if (lba < FAT12_DATA_START_LBA) {
        copy(&s_root[(lba - FAT12_ROOT_START_LBA) * FAT12_SECTOR_SIZE], buf,
             FAT12_SECTOR_SIZE);
        return true;
    }

    cluster = FAT12_FIRST_CLUSTER + (lba - FAT12_DATA_START_LBA);
    if (cluster >= USBS_SETTINGS_CLUSTER && cluster <= WIN_LAST) {
        uint32_t w = cluster - USBS_SETTINGS_CLUSTER;

        copy(&s_win[w * FAT12_SECTOR_SIZE], buf, FAT12_SECTOR_SIZE);
        s_win_written |= (uint64_t)1u << w;
        return true;
    }
    if (cluster >= s_dir_cluster && cluster < s_files_cluster) {
        /* The LECTURES directory is generated afresh on every read. A host
         * that writes it back is only updating an access date: drop it. */
        return true;
    }

    return false;       /* every file but SETTINGS.CSV is read-only */
}

bool usbs_write(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    uint32_t i;

    if (!s_active || (lba + count) > s_vol.total_sectors) {
        return false;
    }

    for (i = 0u; i < count; i++) {
        if (!write_one(lba + i, &buf[i * FAT12_SECTOR_SIZE])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Session end                                                              */
/* ------------------------------------------------------------------------ */

void usbs_end(usbs_result_t *result)
{
    analysis_t a;

    result->outcome = USBS_IMPORT_NONE;
    result->time_set = false;
    result->device_set = false;
    result->device_id = 0u;
    result->session_start = false;
    result->clear_log = false;
    result->module[0] = '\0';
    result->lecture[0] = '\0';
    report_clear(&result->rep, SETF_OK);

    if (!s_active) {
        return;
    }
    s_active = false;

    analyse(&a);
    if (a.kind == A_NONE || !a.changed) {
        return;
    }

    result->rep = a.rep;
    if (a.kind == A_BADFILE || a.rep.status != SETF_OK) {
        result->outcome = USBS_IMPORT_FAILED;
        return;
    }

    /* What needs flash: a new device ID. Do it first, so a failure leaves
     * the clock and the lecture untouched too. */
    if (a.rep.has_device && a.rep.device_id != s_device_id) {
        device_cfg_t cfg = s_cfg;

        if (!devcfg_set_device_id(&cfg, a.rep.device_id)) {
            result->rep.status = SETF_ERR_FLASH;
            result->outcome = USBS_IMPORT_FAILED;
            return;
        }
        result->device_set = true;
        result->device_id = a.rep.device_id;
    }

    result->outcome = USBS_IMPORT_OK;
    result->clear_log = a.rep.clear_log;

    /* Only a #TIME the user actually edited sets the clock: the unedited line
     * shows the moment of attach, which is already in the past by now. */
    if (a.rep.has_time && time_to_epoch(&a.rep.time) != s_shown_epoch) {
        plat_rtc_set(&a.rep.time);
        result->time_set = true;
    }

    /* A new lecture when the names were edited, or #NEWSESSION asked for one.
     * A name the file omits keeps its current value. */
    if (session_would_start(&a.rep)) {
        const char *mod = a.rep.has_module ? a.rep.module : s_cur.module;
        const char *lec = a.rep.has_lecture ? a.rep.lecture : s_cur.lecture;
        uint32_t i;

        for (i = 0u; i < SESS_MODULE_MAX && mod[i] != '\0'; i++) {
            result->module[i] = mod[i];
        }
        result->module[i] = '\0';
        for (i = 0u; i < SESS_LECTURE_MAX && lec[i] != '\0'; i++) {
            result->lecture[i] = lec[i];
        }
        result->lecture[i] = '\0';
        result->session_start = true;
    }
}
