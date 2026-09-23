/*
 * Internals shared between the controller manager's files.
 */
#pragma once

#include "controller_manager.h"

void cm_lock(void);
void cm_unlock(void);

void cm_mapping_db_init(void);
/* cm_mapping_get() with the manager lock already held. */
void cm_mapping_get_locked(uint16_t vid, uint16_t pid, cm_mapping_t *out);
/* Apply a changed remap to connected devices (lock held). */
void cm_remap_connected_locked(uint16_t vid, uint16_t pid, const cm_mapping_t *m);
