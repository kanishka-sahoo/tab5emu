#include "test_util.h"
#include "xpad_proto.h"

/* A 20-byte Xbox 360 input report. */
static void x360_report(uint8_t *d, uint8_t b2, uint8_t b3, uint8_t lt, uint8_t rt, int16_t lx,
                        int16_t ly, int16_t rx, int16_t ry)
{
    memset(d, 0, 20);
    d[0] = 0x00;
    d[1] = 0x14;
    d[2] = b2;
    d[3] = b3;
    d[4] = lt;
    d[5] = rt;
    const int16_t ax[4] = {lx, ly, rx, ry};
    for (int i = 0; i < 4; i++) {
        d[6 + 2 * i] = (uint8_t)(ax[i] & 0xFF);
        d[7 + 2 * i] = (uint8_t)((uint16_t)ax[i] >> 8);
    }
}

/* An 18-byte GIP input report (cmd 0x20). Triggers are 10-bit. */
static void gip_report(uint8_t *d, uint8_t seq, uint8_t b4, uint8_t b5, uint16_t lt, uint16_t rt,
                       int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    memset(d, 0, 18);
    d[0] = 0x20;
    d[1] = 0x00;
    d[2] = seq;
    d[3] = 14;
    d[4] = b4;
    d[5] = b5;
    d[6] = (uint8_t)(lt & 0xFF);
    d[7] = (uint8_t)(lt >> 8);
    d[8] = (uint8_t)(rt & 0xFF);
    d[9] = (uint8_t)(rt >> 8);
    const int16_t ax[4] = {lx, ly, rx, ry};
    for (int i = 0; i < 4; i++) {
        d[10 + 2 * i] = (uint8_t)(ax[i] & 0xFF);
        d[11 + 2 * i] = (uint8_t)((uint16_t)ax[i] >> 8);
    }
}

static void test_match_interface(void)
{
    CHECK(xpad_match_interface(0xFF, 0x5D, 0x01) == XPAD_X360);
    CHECK(xpad_match_interface(0xFF, 0x47, 0xD0) == XPAD_GIP);
    CHECK(xpad_match_interface(0xFF, 0x5D, 0x03) == XPAD_NONE); /* 360 headset */
    CHECK(xpad_match_interface(0x03, 0x00, 0x00) == XPAD_NONE); /* HID */
    CHECK(xpad_match_interface(0x03, 0x47, 0xD0) == XPAD_NONE);
    CHECK(xpad_match_interface(0xFF, 0x47, 0x01) == XPAD_NONE);
    CHECK_STR(xpad_kind_name(XPAD_NONE), "none");
    CHECK(strstr(xpad_kind_name(XPAD_X360), "360") != NULL);
    CHECK(strstr(xpad_kind_name(XPAD_GIP), "GIP") != NULL);
}

static void test_x360_report(void)
{
    uint8_t d[20];
    cm_gamepad_t s;
    memset(&s, 0, sizeof(s));
    /* UP + START; L1 + GUIDE + A(south) + Y(north). */
    x360_report(d, 0x11, 0x95, 64, 63, -32768, 32767, 1234, -1);
    CHECK(xpad_parse(XPAD_X360, d, 20, &s) == XPAD_R_STATE);
    uint32_t want = CM_BIT(CM_BTN_UP) | CM_BIT(CM_BTN_START) | CM_BIT(CM_BTN_L1) |
                    CM_BIT(CM_BTN_GUIDE) | CM_BIT(CM_BTN_SOUTH) | CM_BIT(CM_BTN_NORTH) |
                    CM_BIT(CM_BTN_L2); /* lt 64 is on, rt 63 is off */
    CHECK(s.buttons == want);
    CHECK(s.lt == 64 && s.rt == 63);
    CHECK(s.lx == -32768 && s.ly == 32767 && s.rx == 1234 && s.ry == -1);

    /* The same report again: no change. */
    CHECK(xpad_parse(XPAD_X360, d, 20, &s) == 0);
    /* Triggers crossing the threshold flip L2/R2. */
    x360_report(d, 0x11, 0x95, 10, 255, -32768, 32767, 1234, -1);
    CHECK(xpad_parse(XPAD_X360, d, 20, &s) == XPAD_R_STATE);
    CHECK(!(s.buttons & CM_BIT(CM_BTN_L2)) && (s.buttons & CM_BIT(CM_BTN_R2)));
}

static void test_x360_every_bit(void)
{
    static const struct {
        int byte;
        uint8_t mask;
        cm_button_t btn;
    } bits[] = {
        {2, 0x01, CM_BTN_UP},    {2, 0x02, CM_BTN_DOWN},  {2, 0x04, CM_BTN_LEFT},
        {2, 0x08, CM_BTN_RIGHT}, {2, 0x10, CM_BTN_START}, {2, 0x20, CM_BTN_SELECT},
        {2, 0x40, CM_BTN_L3},    {2, 0x80, CM_BTN_R3},    {3, 0x01, CM_BTN_L1},
        {3, 0x02, CM_BTN_R1},    {3, 0x04, CM_BTN_GUIDE}, {3, 0x10, CM_BTN_SOUTH},
        {3, 0x20, CM_BTN_EAST},  {3, 0x40, CM_BTN_WEST},  {3, 0x80, CM_BTN_NORTH},
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        uint8_t d[20];
        cm_gamepad_t s;
        memset(&s, 0, sizeof(s));
        x360_report(d, bits[i].byte == 2 ? bits[i].mask : 0, bits[i].byte == 3 ? bits[i].mask : 0,
                    0, 0, 0, 0, 0, 0);
        xpad_parse(XPAD_X360, d, 20, &s);
        CHECK(s.buttons == CM_BIT(bits[i].btn));
    }
}

static void test_x360_ignores_other_reports(void)
{
    cm_gamepad_t s = {CM_BIT(CM_BTN_EAST), 1, 2, 3, 4, 5, 6};
    const cm_gamepad_t before = s;
    const uint8_t led_status[3] = {0x01, 0x03, 0x06};
    CHECK(xpad_parse(XPAD_X360, led_status, 3, &s) == 0);
    uint8_t d[20];
    x360_report(d, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0);
    CHECK(xpad_parse(XPAD_X360, d, 13, &s) == 0); /* truncated */
    d[1] = 0x08;
    CHECK(xpad_parse(XPAD_X360, d, 20, &s) == 0); /* wrong length byte */
    d[0] = 0x08;
    d[1] = 0x14;
    CHECK(xpad_parse(XPAD_X360, d, 20, &s) == 0); /* not an input report */
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    CHECK(xpad_parse(XPAD_NONE, d, 20, &s) == 0);
}

static void test_gip_input(void)
{
    uint8_t d[18];
    cm_gamepad_t s;
    memset(&s, 0, sizeof(s));
    /* START + SELECT + B(east) + X(west); DOWN + RIGHT + R1 + L3. */
    gip_report(d, 1, 0x04 | 0x08 | 0x20 | 0x40, 0x02 | 0x08 | 0x20 | 0x40, 1023, 255, 100, -100,
               32767, -32768);
    CHECK(xpad_parse(XPAD_GIP, d, 18, &s) == XPAD_R_STATE);
    uint32_t want = CM_BIT(CM_BTN_START) | CM_BIT(CM_BTN_SELECT) | CM_BIT(CM_BTN_EAST) |
                    CM_BIT(CM_BTN_WEST) | CM_BIT(CM_BTN_DOWN) | CM_BIT(CM_BTN_RIGHT) |
                    CM_BIT(CM_BTN_R1) | CM_BIT(CM_BTN_L3) | CM_BIT(CM_BTN_L2);
    CHECK(s.buttons == want);
    CHECK(s.lt == 255 && s.rt == 63); /* 10-bit >> 2; 63 is below the threshold */
    CHECK(s.lx == 100 && s.ly == -100 && s.rx == 32767 && s.ry == -32768);
    CHECK(xpad_parse(XPAD_GIP, d, 18, &s) == 0);

    gip_report(d, 2, 0x10 | 0x80, 0x01 | 0x04 | 0x10 | 0x80, 0, 256, 0, 0, 0, 0);
    CHECK(xpad_parse(XPAD_GIP, d, 18, &s) == XPAD_R_STATE);
    CHECK(s.buttons == (CM_BIT(CM_BTN_SOUTH) | CM_BIT(CM_BTN_NORTH) | CM_BIT(CM_BTN_UP) |
                        CM_BIT(CM_BTN_LEFT) | CM_BIT(CM_BTN_L1) | CM_BIT(CM_BTN_R3) |
                        CM_BIT(CM_BTN_R2)));
    CHECK(s.rt == 64);

    /* Too short for an input report: ignored. */
    const cm_gamepad_t before = s;
    gip_report(d, 3, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0);
    CHECK(xpad_parse(XPAD_GIP, d, 17, &s) == 0);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_gip_guide_and_ack(void)
{
    cm_gamepad_t s;
    memset(&s, 0, sizeof(s));
    /* Guide is its own message, flagged "ack required". */
    const uint8_t guide_down[6] = {0x07, 0x30, 0x05, 0x02, 0x01, 0x5B};
    CHECK(xpad_parse(XPAD_GIP, guide_down, 6, &s) == (XPAD_R_STATE | XPAD_R_ACK));
    CHECK(s.buttons == CM_BIT(CM_BTN_GUIDE));

    /* Input reports keep the guide bit. */
    uint8_t d[18];
    gip_report(d, 6, 0x10, 0, 0, 0, 0, 0, 0, 0);
    CHECK(xpad_parse(XPAD_GIP, d, 18, &s) == XPAD_R_STATE);
    CHECK(s.buttons == (CM_BIT(CM_BTN_GUIDE) | CM_BIT(CM_BTN_SOUTH)));

    /* Repeated guide-down: ack still requested, no state change. */
    CHECK(xpad_parse(XPAD_GIP, guide_down, 6, &s) == XPAD_R_ACK);

    const uint8_t guide_up[6] = {0x07, 0x30, 0x07, 0x02, 0x00, 0x5B};
    CHECK(xpad_parse(XPAD_GIP, guide_up, 6, &s) == (XPAD_R_STATE | XPAD_R_ACK));
    CHECK(s.buttons == CM_BIT(CM_BTN_SOUTH));

    /* Any message with the ack option asks for one, known or not. */
    const uint8_t other[4] = {0x03, 0x10, 0x01, 0x00};
    CHECK(xpad_parse(XPAD_GIP, other, 4, &s) == XPAD_R_ACK);
    const uint8_t no_ack[4] = {0x03, 0x20, 0x01, 0x00};
    CHECK(xpad_parse(XPAD_GIP, no_ack, 4, &s) == 0);
    CHECK(xpad_parse(XPAD_GIP, no_ack, 3, &s) == 0); /* runt */
}

static void test_gip_ack_packet(void)
{
    const uint8_t guide_down[6] = {0x07, 0x30, 0x2A, 0x02, 0x01, 0x5B};
    uint8_t out[XPAD_PACKET_MAX];
    size_t n = xpad_gip_ack(guide_down, sizeof(guide_down), out);
    const uint8_t want[13] = {0x01, 0x20, 0x2A, 0x09, 0x00, 0x07, 0x20, 0x02, 0, 0, 0, 0, 0};
    CHECK(n == sizeof(want));
    CHECK(memcmp(out, want, sizeof(want)) == 0);
    CHECK(xpad_gip_ack(guide_down, 3, out) == 0);
}

static void test_gip_power_on(void)
{
    uint8_t out[XPAD_PACKET_MAX];
    const uint8_t want[5] = {0x05, 0x20, 0x09, 0x01, 0x00};
    CHECK(xpad_gip_power_on(0x09, out) == sizeof(want));
    CHECK(memcmp(out, want, sizeof(want)) == 0);
}

static void test_x360_led(void)
{
    uint8_t out[XPAD_PACKET_MAX];
    for (int p = 0; p < 4; p++) {
        CHECK(xpad_x360_led(p, out) == 3);
        CHECK(out[0] == 0x01 && out[1] == 0x03 && out[2] == 0x06 + p);
    }
}

static void test_rumble_packets(void)
{
    uint8_t out[XPAD_PACKET_MAX];
    CHECK(xpad_rumble(XPAD_X360, 0, 0xC0, 0x40, out) == 8);
    const uint8_t x360[8] = {0x00, 0x08, 0x00, 0xC0, 0x40, 0x00, 0x00, 0x00};
    CHECK(memcmp(out, x360, sizeof(x360)) == 0);

    CHECK(xpad_rumble(XPAD_GIP, 0x11, 255, 0, out) == 13);
    CHECK(out[0] == 0x09 && out[2] == 0x11 && out[3] == 0x09);
    CHECK(out[5] == 0x0F);                  /* all motors enabled */
    CHECK(out[6] == 0 && out[7] == 0);      /* trigger motors off */
    CHECK(out[8] == 100 && out[9] == 0);    /* percent */
    CHECK(out[10] == 0xFF && out[12] == 0xFF);
    xpad_rumble(XPAD_GIP, 0x12, 0, 128, out);
    CHECK(out[8] == 0 && out[9] == 50);

    CHECK(xpad_rumble(XPAD_NONE, 0, 255, 255, out) == 0);
}

int main(void)
{
    RUN(test_match_interface);
    RUN(test_x360_report);
    RUN(test_x360_every_bit);
    RUN(test_x360_ignores_other_reports);
    RUN(test_gip_input);
    RUN(test_gip_guide_and_ack);
    RUN(test_gip_ack_packet);
    RUN(test_gip_power_on);
    RUN(test_x360_led);
    RUN(test_rumble_packets);
    return TEST_EXIT();
}
