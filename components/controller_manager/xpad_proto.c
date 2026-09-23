#include "xpad_proto.h"

#include <string.h>

#define GIP_CMD_ACK 0x01
#define GIP_CMD_POWER 0x05
#define GIP_CMD_GUIDE 0x07
#define GIP_CMD_RUMBLE 0x09
#define GIP_CMD_INPUT 0x20
#define GIP_OPT_ACK 0x10
#define GIP_OPT_INTERNAL 0x20

xpad_kind_t xpad_match_interface(uint8_t cls, uint8_t subclass, uint8_t protocol)
{
    if (cls != 0xFF) {
        return XPAD_NONE;
    }
    if (subclass == 0x5D && protocol == 0x01) {
        return XPAD_X360;
    }
    if (subclass == 0x47 && protocol == 0xD0) {
        return XPAD_GIP;
    }
    return XPAD_NONE;
}

const char *xpad_kind_name(xpad_kind_t k)
{
    return k == XPAD_GIP ? "Xbox One/Series (GIP)" : k == XPAD_X360 ? "Xbox 360 (XInput)" : "none";
}

static int16_t le16s(const uint8_t *p)
{
    return (int16_t)(p[0] | (p[1] << 8));
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* bit -> CM button, for a byte of flags. */
typedef struct {
    uint8_t byte, mask, btn;
} bitmap_t;

static uint32_t map_bits(const uint8_t *d, const bitmap_t *m, size_t n)
{
    uint32_t b = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[m[i].byte] & m[i].mask) {
            b |= CM_BIT(m[i].btn);
        }
    }
    return b;
}

static const bitmap_t s_x360_bits[] = {
    {2, 0x01, CM_BTN_UP},    {2, 0x02, CM_BTN_DOWN},  {2, 0x04, CM_BTN_LEFT},
    {2, 0x08, CM_BTN_RIGHT}, {2, 0x10, CM_BTN_START}, {2, 0x20, CM_BTN_SELECT},
    {2, 0x40, CM_BTN_L3},    {2, 0x80, CM_BTN_R3},    {3, 0x01, CM_BTN_L1},
    {3, 0x02, CM_BTN_R1},    {3, 0x04, CM_BTN_GUIDE}, {3, 0x10, CM_BTN_SOUTH},
    {3, 0x20, CM_BTN_EAST},  {3, 0x40, CM_BTN_WEST},  {3, 0x80, CM_BTN_NORTH},
};

static const bitmap_t s_gip_bits[] = {
    {4, 0x04, CM_BTN_START}, {4, 0x08, CM_BTN_SELECT}, {4, 0x10, CM_BTN_SOUTH},
    {4, 0x20, CM_BTN_EAST},  {4, 0x40, CM_BTN_WEST},   {4, 0x80, CM_BTN_NORTH},
    {5, 0x01, CM_BTN_UP},    {5, 0x02, CM_BTN_DOWN},   {5, 0x04, CM_BTN_LEFT},
    {5, 0x08, CM_BTN_RIGHT}, {5, 0x10, CM_BTN_L1},     {5, 0x20, CM_BTN_R1},
    {5, 0x40, CM_BTN_L3},    {5, 0x80, CM_BTN_R3},
};

#define TRIGGER_ON 64 /* of 255: digital L2/R2 threshold */

static void set_triggers(cm_gamepad_t *s, uint8_t lt, uint8_t rt)
{
    s->lt = lt;
    s->rt = rt;
    s->buttons &= ~(CM_BIT(CM_BTN_L2) | CM_BIT(CM_BTN_R2));
    s->buttons |= (lt >= TRIGGER_ON ? CM_BIT(CM_BTN_L2) : 0) | (rt >= TRIGGER_ON ? CM_BIT(CM_BTN_R2) : 0);
}

static unsigned changed(const cm_gamepad_t *a, const cm_gamepad_t *b)
{
    return memcmp(a, b, sizeof(*a)) != 0 ? XPAD_R_STATE : 0;
}

static unsigned parse_x360(const uint8_t *d, size_t n, cm_gamepad_t *s)
{
    /* 0x00 0x14: input report. Others (LED/rumble status, 0x08 connect on
     * wireless receivers) carry no input. */
    if (n < 14 || d[0] != 0x00 || d[1] < 0x14) {
        return 0;
    }
    cm_gamepad_t o = *s;
    s->buttons = map_bits(d, s_x360_bits, sizeof(s_x360_bits) / sizeof(s_x360_bits[0]));
    set_triggers(s, d[4], d[5]);
    s->lx = le16s(d + 6);
    s->ly = le16s(d + 8);
    s->rx = le16s(d + 10);
    s->ry = le16s(d + 12);
    return changed(&o, s);
}

static unsigned parse_gip(const uint8_t *d, size_t n, cm_gamepad_t *s)
{
    if (n < 4) {
        return 0;
    }
    unsigned r = (d[1] & GIP_OPT_ACK) ? XPAD_R_ACK : 0;
    cm_gamepad_t o = *s;
    if (d[0] == GIP_CMD_INPUT && n >= 18) {
        uint32_t guide = s->buttons & CM_BIT(CM_BTN_GUIDE); /* reported separately */
        s->buttons = guide | map_bits(d, s_gip_bits, sizeof(s_gip_bits) / sizeof(s_gip_bits[0]));
        /* Triggers are 10-bit. */
        set_triggers(s, (uint8_t)(le16(d + 6) >> 2), (uint8_t)(le16(d + 8) >> 2));
        s->lx = le16s(d + 10);
        s->ly = le16s(d + 12);
        s->rx = le16s(d + 14);
        s->ry = le16s(d + 16);
    } else if (d[0] == GIP_CMD_GUIDE && n >= 5) {
        s->buttons = (s->buttons & ~CM_BIT(CM_BTN_GUIDE)) | ((d[4] & 0x01) ? CM_BIT(CM_BTN_GUIDE) : 0);
    }
    return r | changed(&o, s);
}

unsigned xpad_parse(xpad_kind_t kind, const uint8_t *d, size_t n, cm_gamepad_t *state)
{
    return kind == XPAD_X360 ? parse_x360(d, n, state) : kind == XPAD_GIP ? parse_gip(d, n, state) : 0;
}

size_t xpad_gip_ack(const uint8_t *report, size_t n, uint8_t *out)
{
    if (n < 4) {
        return 0;
    }
    const uint8_t ack[13] = {GIP_CMD_ACK, GIP_OPT_INTERNAL, report[2], 0x09, 0x00, report[0],
                             GIP_OPT_INTERNAL, report[3], 0, 0, 0, 0, 0};
    memcpy(out, ack, sizeof(ack));
    return sizeof(ack);
}

size_t xpad_gip_power_on(uint8_t seq, uint8_t *out)
{
    const uint8_t p[5] = {GIP_CMD_POWER, GIP_OPT_INTERNAL, seq, 0x01, 0x00};
    memcpy(out, p, sizeof(p));
    return sizeof(p);
}

size_t xpad_x360_led(int player, uint8_t *out)
{
    /* 0x06..0x09: quadrant 1..4 lit steadily. */
    const uint8_t p[3] = {0x01, 0x03, (uint8_t)(0x06 + (player & 3))};
    memcpy(out, p, sizeof(p));
    return sizeof(p);
}

size_t xpad_rumble(xpad_kind_t kind, uint8_t seq, uint8_t strong, uint8_t weak, uint8_t *out)
{
    if (kind == XPAD_GIP) {
        /* Motor magnitudes are percent; duration/repeat 0xFF = until
         * changed. Trigger motors off. */
        const uint8_t p[13] = {GIP_CMD_RUMBLE, 0x00, seq, 0x09, 0x00, 0x0F, 0x00, 0x00,
                               (uint8_t)(strong * 100 / 255), (uint8_t)(weak * 100 / 255),
                               0xFF, 0x00, 0xFF};
        memcpy(out, p, sizeof(p));
        return sizeof(p);
    }
    if (kind == XPAD_X360) {
        const uint8_t p[8] = {0x00, 0x08, 0x00, strong, weak, 0x00, 0x00, 0x00};
        memcpy(out, p, sizeof(p));
        return sizeof(p);
    }
    return 0;
}
