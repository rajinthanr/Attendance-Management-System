/**
 * @file    app_types.h
 * @brief   Level 2 (logic) — data types shared across the application and,
 *          where unavoidable, with the Level 1 port layer.
 *
 * This header must stay free of any vendor/HAL include. It is compiled both
 * for the target and for the host test harness.
 */
#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Seconds since APP_EPOCH_YEAR (2000-01-01T00:00:00Z). */
typedef uint32_t app_epoch_t;

/**
 * Broken-down calendar time.
 *
 * Level 1 reads/writes this straight out of the RTC calendar registers; it
 * performs no conversion. All epoch<->calendar arithmetic is Level 2
 * (see timeutil.c).
 */
typedef struct {
    uint16_t year;   /**< Full year, e.g. 2026. */
    uint8_t  month;  /**< 1..12 */
    uint8_t  day;    /**< 1..31 */
    uint8_t  hour;   /**< 0..23 */
    uint8_t  minute; /**< 0..59 */
    uint8_t  second; /**< 0..59 */
} app_datetime_t;

/**
 * One attendance record.
 *
 * Exactly 8 bytes so it maps 1:1 onto an STM32L4 flash double-word, the
 * smallest programmable unit. Keeping the record and the programming
 * granularity identical removes read-modify-write entirely from log_store.
 */
typedef struct {
    uint32_t student_id;  /**< 32-bit EM4100 unique ID. */
    app_epoch_t stamp;    /**< Seconds since epoch. */
} app_record_t;

/** Decoded EM4100 tag contents. */
typedef struct {
    uint8_t  version;     /**< 8-bit customer/version field. */
    uint32_t unique_id;   /**< 32-bit unique ID. */
} app_tag_t;

/** Outcome of presenting a card, used to pick the feedback pattern. */
typedef enum {
    APP_SCAN_ACCEPTED = 0,  /**< Known student, recorded. */
    APP_SCAN_DUPLICATE,     /**< Same ID inside the dedup window. */
    APP_SCAN_UNKNOWN,       /**< Valid tag, not on the student list. */
    APP_SCAN_NO_CARD        /**< Touch fired but nothing decodable. */
} app_scan_result_t;

/** Why the MCU came out of reset. Level 1 maps the RCC reset flags onto this. */
typedef enum {
    APP_BOOT_POWER_ON = 0,
    APP_BOOT_FROM_STANDBY,
    APP_BOOT_WATCHDOG,
    APP_BOOT_OTHER
} app_boot_cause_t;

/** Raw ADC material for the battery estimate. No scaling applied by Level 1. */
typedef struct {
    uint16_t vbat_counts;    /**< Divider node, 12-bit right aligned. */
    uint16_t vrefint_counts; /**< Internal reference, same sampling run. */
    uint16_t vrefint_cal;    /**< Factory VREFINT_CAL value from system memory. */
} app_adc_sample_t;

#endif /* APP_TYPES_H */
