/**
 * @file    app_debug.c
 * @brief   Level 2 (logic) — storage for the live-debug globals.
 */
#include "app_debug.h"

volatile uint32_t dbg_battery_mv;
volatile uint8_t  dbg_battery_state;
volatile uint16_t dbg_battery_counts;

volatile uint32_t dbg_card_id;
volatile uint8_t  dbg_card_uid[10];
volatile uint8_t  dbg_card_uid_len;
volatile uint8_t  dbg_card_atqa[2];
volatile uint8_t  dbg_card_sak;
volatile uint32_t dbg_card_count;
volatile uint8_t  dbg_scan_result;

volatile bool     dbg_button_down;
volatile uint32_t dbg_button_short_count;
volatile uint32_t dbg_button_long_count;

volatile bool     dbg_nfc_ready;
volatile uint8_t  dbg_nfc_chip_id;
volatile bool     dbg_nfc_supply_3v3;
volatile uint8_t  dbg_nfc_amplitude;
volatile uint8_t  dbg_nfc_last_status;
volatile uint32_t dbg_nfc_polls;
volatile uint32_t dbg_nfc_errors;
volatile uint32_t dbg_nfc_collisions;

volatile uint8_t  dbg_state;
volatile uint8_t  dbg_boot_cause;
volatile uint32_t dbg_uptime_ms;
volatile bool     dbg_vbus;
volatile bool     dbg_usb_host;
volatile uint32_t dbg_records_ram;
volatile uint32_t dbg_records_flash;
volatile uint32_t dbg_records_free;
volatile uint32_t dbg_students;
volatile app_datetime_t dbg_now;

volatile app_datetime_t dbg_set_time;
volatile bool     dbg_set_time_request;

void app_debug_reset(void)
{
    static const app_datetime_t zero_time = { 0u, 0u, 0u, 0u, 0u, 0u };
    uint8_t i;

    dbg_battery_mv = 0u;
    dbg_battery_state = 0u;
    dbg_battery_counts = 0u;

    dbg_card_id = 0u;
    for (i = 0u; i < sizeof(dbg_card_uid); i++) {
        dbg_card_uid[i] = 0u;
    }
    dbg_card_uid_len = 0u;
    dbg_card_atqa[0] = 0u;
    dbg_card_atqa[1] = 0u;
    dbg_card_sak = 0u;
    dbg_card_count = 0u;
    dbg_scan_result = 0u;

    dbg_button_down = false;
    dbg_button_short_count = 0u;
    dbg_button_long_count = 0u;

    dbg_nfc_ready = false;
    dbg_nfc_chip_id = 0u;
    dbg_nfc_supply_3v3 = false;
    dbg_nfc_amplitude = 0u;
    dbg_nfc_last_status = 0u;
    dbg_nfc_polls = 0u;
    dbg_nfc_errors = 0u;
    dbg_nfc_collisions = 0u;

    dbg_state = 0u;
    dbg_boot_cause = 0u;
    dbg_uptime_ms = 0u;
    dbg_vbus = false;
    dbg_usb_host = false;
    dbg_records_ram = 0u;
    dbg_records_flash = 0u;
    dbg_records_free = 0u;
    dbg_students = 0u;
    dbg_now = zero_time;

    dbg_set_time = zero_time;
    dbg_set_time_request = false;
}
