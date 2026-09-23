/*
 * Xbox pad wire protocols (plan D11), platform-neutral so the parsers can be
 * unit tested on the host.
 *
 *  - Xbox 360 XInput: interface 0xFF/0x5D/0x01, 20-byte input report.
 *  - Xbox One / Series GIP: interface 0xFF/0x47/0xD0; the pad sends nothing
 *    until it gets the power-on packet, and messages flagged "ack required"
 *    (e.g. the Guide button) must be acknowledged.
 *
 * Byte layouts follow the public GIP/XInput documentation, with Linux xpad
 * used as a protocol reference only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_manager.h"

typedef enum {
    XPAD_NONE = 0,
    XPAD_X360,
    XPAD_GIP,
} xpad_kind_t;

#define XPAD_PACKET_MAX 64

xpad_kind_t xpad_match_interface(uint8_t cls, uint8_t subclass, uint8_t protocol);
const char *xpad_kind_name(xpad_kind_t k);

enum {
    XPAD_R_STATE = 1u << 0, /* *state changed */
    XPAD_R_ACK = 1u << 1,   /* GIP: send xpad_gip_ack() for this report */
};

/* Parse one input report into *state (fields the report doesn't carry are
 * kept). Returns XPAD_R_* flags. */
unsigned xpad_parse(xpad_kind_t kind, const uint8_t *d, size_t n, cm_gamepad_t *state);

/* Packet builders; return the length written to out (<= XPAD_PACKET_MAX). */
size_t xpad_gip_ack(const uint8_t *report, size_t n, uint8_t *out);
size_t xpad_gip_power_on(uint8_t seq, uint8_t *out);
/* 360 ring LED for player 0..3. */
size_t xpad_x360_led(int player, uint8_t *out);
size_t xpad_rumble(xpad_kind_t kind, uint8_t seq, uint8_t strong, uint8_t weak, uint8_t *out);
