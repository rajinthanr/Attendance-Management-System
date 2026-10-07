/**
 * @file    device_cfg.h
 * @brief   Level 2 (logic) — what the device keeps in flash besides the log:
 *          its ID.
 *
 * The device keeps no list of registered cards. It records every card it
 * reads; who a card belongs to, and so whether it is registered at all, is
 * decided by the PC app from its own database.
 *
 * The ID is what the host sees as the volume serial number and what the app
 * uses to tell units apart.
 */
#ifndef DEVICE_CFG_H
#define DEVICE_CFG_H

#include "app_types.h"
#include "nv_layout.h"

typedef struct {
    bool     valid;       /**< The config page holds a readable configuration. */
    uint32_t device_id;   /**< 0 when unset. */
} device_cfg_t;

/** Read the config page. A blank or old-format page gives valid == false, id 0. */
void devcfg_load(device_cfg_t *c);

/**
 * Replace the device ID: erase the config page, program it, read it back.
 * @return true only when the stored value reads back as @p id.
 */
bool devcfg_set_device_id(device_cfg_t *c, uint32_t id);

#endif /* DEVICE_CFG_H */
