/*
 * The controller manager has global state and no deinit, so every test
 * disconnects the devices it connected before returning.
 */
#include "controller_manager.h"
#include "retro_os.h"
#include "test_util.h"

/* A fake device: records the callbacks it receives. */
typedef struct {
    int player;       /* last set_player, -2 if never called */
    int set_calls;
    int rumbles;
    uint8_t strong, weak;
    cm_gamepad_t polled; /* state returned by poll(), if used */
} fake_t;

static void fake_set_player(void *ctx, int player)
{
    fake_t *f = ctx;
    f->player = player;
    f->set_calls++;
}

static void fake_rumble(void *ctx, uint8_t strong, uint8_t weak)
{
    fake_t *f = ctx;
    f->rumbles++;
    f->strong = strong;
    f->weak = weak;
}

static void fake_poll(void *ctx, cm_gamepad_t *state)
{
    *state = ((fake_t *)ctx)->polled;
}

static int connect_pad(fake_t *f, const char *name, uint16_t vid, uint16_t pid)
{
    memset(f, 0, sizeof(*f));
    f->player = -2;
    cm_device_desc_t d = {
        .kind = CM_SRC_XINPUT,
        .name = name,
        .vid = vid,
        .pid = pid,
        .rumble = fake_rumble,
        .set_player = fake_set_player,
        .ctx = f,
    };
    return cm_device_connect(&d);
}

static int connect_builtin(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->player = -2;
    cm_device_desc_t d = {
        .kind = CM_SRC_KEYBOARD,
        .name = "keys",
        .builtin = true,
        .set_player = fake_set_player,
        .ctx = f,
    };
    return cm_device_connect(&d);
}

static void press(int id, uint32_t buttons)
{
    cm_gamepad_t s;
    memset(&s, 0, sizeof(s));
    s.buttons = buttons;
    cm_device_update(id, &s);
}

static void stick(int id, int16_t lx, int16_t ly)
{
    cm_gamepad_t s;
    memset(&s, 0, sizeof(s));
    s.lx = lx;
    s.ly = ly;
    cm_device_update(id, &s);
}

static retro_input_state_t poll_input(void)
{
    retro_input_state_t in;
    cm_poll(&in);
    return in;
}

static void test_no_devices(void)
{
    retro_input_state_t in = poll_input();
    CHECK(in.connected == 0 && in.hotkeys == 0);
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        CHECK(in.pads[p].buttons == 0);
    }
    CHECK(cm_touch_overlay_visible());
    CHECK(cm_device_player(0) == -1);
    CHECK(cm_device_player(-1) == -1);
    CHECK(cm_device_player(CM_MAX_DEVICES) == -1);
}

static void test_players_in_connect_order(void)
{
    fake_t f[3];
    int id[3];
    for (int i = 0; i < 3; i++) {
        id[i] = connect_pad(&f[i], "pad", 0x1111, 0x2222);
        CHECK(id[i] >= 0);
        CHECK(cm_device_player(id[i]) == i);
        CHECK(f[i].player == i && f[i].set_calls == 1);
    }
    CHECK(poll_input().connected == 0x7);
    CHECK(!cm_touch_overlay_visible());
    cm_stats_t st;
    cm_get_stats(&st);
    CHECK(st.devices == 3 && st.pads == 3);
    CHECK_STR(st.names[2], "pad");
    CHECK_STR(st.names[3], "");
    for (int i = 0; i < 3; i++) {
        cm_device_disconnect(id[i]);
    }
    CHECK(poll_input().connected == 0);
    CHECK(cm_touch_overlay_visible());
}

static void test_disconnect_reassigns_oldest(void)
{
    fake_t f[6];
    int id[6];
    for (int i = 0; i < 6; i++) {
        id[i] = connect_pad(&f[i], "pad", 0x1111, 0x2222);
    }
    /* Four slots: the fifth and sixth wait. */
    CHECK(cm_device_player(id[3]) == 3);
    CHECK(cm_device_player(id[4]) == -1 && cm_device_player(id[5]) == -1);
    CHECK(f[4].set_calls == 0);

    /* Player 2 leaves: the oldest waiting pad (the fifth) takes slot 2. */
    cm_device_disconnect(id[1]);
    CHECK(cm_device_player(id[4]) == 1);
    CHECK(f[4].player == 1 && f[4].set_calls == 1);
    CHECK(cm_device_player(id[5]) == -1);
    /* Others keep their slots and aren't re-notified. */
    CHECK(f[0].set_calls == 1 && f[2].set_calls == 1 && f[3].set_calls == 1);
    CHECK(poll_input().connected == 0xF);

    cm_device_disconnect(id[0]);
    CHECK(cm_device_player(id[5]) == 0);
    /* A reused device id starts fresh. */
    fake_t g;
    int gid = connect_pad(&g, "late", 0x1111, 0x2222);
    CHECK(gid >= 0 && cm_device_player(gid) == -1);

    cm_device_disconnect(gid);
    for (int i = 2; i < 6; i++) {
        cm_device_disconnect(id[i]);
    }
    cm_device_disconnect(id[2]); /* twice: ignored */
    CHECK(poll_input().connected == 0);
}

static void test_device_limit(void)
{
    fake_t f[CM_MAX_DEVICES + 1];
    int id[CM_MAX_DEVICES];
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        id[i] = connect_pad(&f[i], "pad", 1, 2);
        CHECK(id[i] >= 0);
    }
    CHECK(connect_pad(&f[CM_MAX_DEVICES], "extra", 1, 2) == -1);
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        cm_device_disconnect(id[i]);
    }
}

static void test_builtin_feeds_player_one(void)
{
    fake_t kb, pad;
    int k = connect_builtin(&kb);
    CHECK(cm_device_player(k) == 0);
    CHECK(kb.set_calls == 0);
    CHECK(cm_touch_overlay_visible());

    /* Doesn't take a slot: the first pad is still player 1. */
    int p = connect_pad(&pad, "pad", 1, 2);
    CHECK(cm_device_player(p) == 0);
    CHECK(!cm_touch_overlay_visible());

    press(k, CM_BIT(CM_BTN_EAST));
    press(p, CM_BIT(CM_BTN_SOUTH));
    retro_input_state_t in = poll_input();
    CHECK(in.pads[0].buttons == (RETRO_BTN_A | RETRO_BTN_B)); /* merged */
    CHECK(in.connected == 0x1); /* from the pad only */

    cm_device_disconnect(p);
    in = poll_input();
    CHECK(in.pads[0].buttons == RETRO_BTN_A);
    CHECK(in.connected == 0);
    cm_stats_t st;
    cm_get_stats(&st);
    CHECK(st.devices == 1 && st.pads == 0);
    cm_device_disconnect(k);
}

static void test_default_positional_mapping(void)
{
    static const struct {
        cm_button_t phys;
        uint16_t btn;
    } map[] = {
        {CM_BTN_UP, RETRO_BTN_UP},       {CM_BTN_DOWN, RETRO_BTN_DOWN},
        {CM_BTN_LEFT, RETRO_BTN_LEFT},   {CM_BTN_RIGHT, RETRO_BTN_RIGHT},
        {CM_BTN_EAST, RETRO_BTN_A},      {CM_BTN_SOUTH, RETRO_BTN_B},
        {CM_BTN_NORTH, RETRO_BTN_X},     {CM_BTN_WEST, RETRO_BTN_Y},
        {CM_BTN_L1, RETRO_BTN_L},        {CM_BTN_R1, RETRO_BTN_R},
        {CM_BTN_START, RETRO_BTN_START}, {CM_BTN_SELECT, RETRO_BTN_SELECT},
        {CM_BTN_L2, 0},                  {CM_BTN_R2, 0},
        {CM_BTN_L3, 0},                  {CM_BTN_R3, 0},
        {CM_BTN_GUIDE, 0},
    };
    fake_t a, b;
    int ia = connect_pad(&a, "p1", 1, 2);
    int ib = connect_pad(&b, "p2", 1, 2);
    CHECK(cm_device_player(ib) == 1);
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        press(ib, CM_BIT(map[i].phys));
        retro_input_state_t in = poll_input();
        CHECK(in.pads[1].buttons == map[i].btn);
        CHECK(in.pads[0].buttons == 0);
        CHECK(in.hotkeys == (map[i].phys == CM_BTN_GUIDE ? RETRO_HOTKEY_MENU : 0u));
    }
    press(ib, 0);
    CHECK(poll_input().hotkeys == 0);
    cm_device_disconnect(ib);
    cm_device_disconnect(ia);
}

static void test_axes_copied(void)
{
    fake_t a, b;
    int ia = connect_pad(&a, "a", 1, 2);
    int ib = connect_pad(&b, "b", 1, 2);
    cm_gamepad_t s = {0, 100, -200, 300, -400, 0, 0};
    cm_device_update(ib, &s);
    retro_input_state_t in = poll_input();
    CHECK(in.axes[1][0] == 100 && in.axes[1][1] == -200);
    CHECK(in.axes[1][2] == 300 && in.axes[1][3] == -400);
    CHECK(in.axes[0][0] == 0 && in.axes[0][1] == 0);
    cm_stats_t st;
    cm_get_stats(&st);
    CHECK(st.updates > 0);
    cm_device_disconnect(ia);
    cm_device_disconnect(ib);
}

static void test_stick_dpad_hysteresis(void)
{
    fake_t f;
    int id = connect_pad(&f, "pad", 1, 2);
    stick(id, 16384, 0);
    CHECK(poll_input().pads[0].buttons == 0); /* at, not past, the on point */
    stick(id, 16385, 0);
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_RIGHT);
    stick(id, 12001, 0);
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_RIGHT); /* held: off point applies */
    stick(id, 12000, 0);
    CHECK(poll_input().pads[0].buttons == 0);
    stick(id, 13000, 0);
    CHECK(poll_input().pads[0].buttons == 0); /* released: needs the on point again */

    stick(id, -20000, 20000);
    CHECK(poll_input().pads[0].buttons == (RETRO_BTN_LEFT | RETRO_BTN_UP));
    stick(id, -13000, -30000);
    CHECK(poll_input().pads[0].buttons == (RETRO_BTN_LEFT | RETRO_BTN_DOWN));
    stick(id, 0, 0);
    CHECK(poll_input().pads[0].buttons == 0);
    cm_device_disconnect(id);
}

static void test_poll_callback(void)
{
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.polled.buttons = CM_BIT(CM_BTN_START);
    cm_device_desc_t d = {.name = "polled", .builtin = true, .poll = fake_poll, .ctx = &f};
    int id = cm_device_connect(&d);
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_START);
    f.polled.buttons = 0;
    CHECK(poll_input().pads[0].buttons == 0);
    cm_device_disconnect(id);
}

static void test_rumble_dispatch(void)
{
    fake_t a, b;
    int ia = connect_pad(&a, "a", 1, 2);
    int ib = connect_pad(&b, "b", 1, 2);
    cm_rumble(1, 200, 50);
    CHECK(a.rumbles == 0);
    CHECK(b.rumbles == 1 && b.strong == 200 && b.weak == 50);
    cm_rumble(3, 1, 1); /* nobody there */
    CHECK(a.rumbles == 0 && b.rumbles == 1);
    cm_device_disconnect(ia);
    cm_device_disconnect(ib);
}

static bool mapping_eq(const cm_mapping_t *a, const cm_mapping_t *b)
{
    for (int i = 0; i < CM_BTN_COUNT; i++) {
        if (a->buttons[i] != b->buttons[i] || a->hotkeys[i] != b->hotkeys[i]) {
            return false;
        }
    }
    return a->stick_dpad == b->stick_dpad;
}

static const char s_db_json[] =
    "{\n"
    "  \"version\": 1,\n"
    "  \"controllers\": [\n"
    "    { \"id\": \"045e:0b12\", \"name\": \"Xbox Series\", \"stick_dpad\": false,\n"
    "      \"map\": { \"south\": \"a\", \"east\": \"b\", \"guide\": \"none\",\n"
    "                 \"select\": \"menu\", \"l2\": \"l\", \"bogus\": \"x\" } },\n"
    "    { \"id\": \"not-an-id\" },\n"
    "    { \"id\": \"0e6f:02a4\", \"map\": { \"west\": \"x\", \"north\": \"y\" } }\n"
    "  ]\n"
    "}\n";

static void test_mapping_parse_applies_on_connect(void)
{
    CHECK(cm_mapping_parse(s_db_json, strlen(s_db_json)));
    cm_mapping_t m;
    cm_mapping_get(0x045e, 0x0b12, &m);
    CHECK(m.buttons[CM_BTN_SOUTH] == RETRO_BTN_A && m.buttons[CM_BTN_EAST] == RETRO_BTN_B);
    CHECK(m.hotkeys[CM_BTN_GUIDE] == 0 && m.hotkeys[CM_BTN_SELECT] == RETRO_HOTKEY_MENU);
    CHECK(m.buttons[CM_BTN_SELECT] == 0);
    CHECK(m.buttons[CM_BTN_L2] == RETRO_BTN_L);
    CHECK(m.buttons[CM_BTN_NORTH] == RETRO_BTN_X); /* unlisted: default */
    CHECK(!m.stick_dpad);

    fake_t f;
    int id = connect_pad(&f, "series", 0x045e, 0x0b12);
    press(id, CM_BIT(CM_BTN_SOUTH));
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_A);
    press(id, CM_BIT(CM_BTN_GUIDE));
    retro_input_state_t in = poll_input();
    CHECK(in.pads[0].buttons == 0 && in.hotkeys == 0);
    press(id, CM_BIT(CM_BTN_SELECT));
    in = poll_input();
    CHECK(in.pads[0].buttons == 0 && in.hotkeys == RETRO_HOTKEY_MENU);
    stick(id, 32767, 0); /* stick_dpad off */
    CHECK(poll_input().pads[0].buttons == 0);
    cm_device_disconnect(id);

    /* Other pads keep the default. */
    cm_mapping_t def, other;
    cm_mapping_default(&def);
    cm_mapping_get(0x1234, 0x5678, &other);
    CHECK(mapping_eq(&def, &other));
    cm_mapping_get(0x0e6f, 0x02a4, &other);
    CHECK(other.buttons[CM_BTN_WEST] == RETRO_BTN_X && other.buttons[CM_BTN_NORTH] == RETRO_BTN_Y);
}

static void test_mapping_parse_rejects(void)
{
    const char *bad[] = {"", "{", "[]", "{\"controllers\": {}}", "{\"version\": 1}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!cm_mapping_parse(bad[i], strlen(bad[i])));
    }
}

static void test_mapping_live_apply(void)
{
    fake_t f;
    int id = connect_pad(&f, "live", 0xABCD, 0x0001);
    press(id, CM_BIT(CM_BTN_EAST));
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_A);

    cm_mapping_t m;
    cm_mapping_default(&m);
    m.buttons[CM_BTN_EAST] = RETRO_BTN_START;
    CHECK(cm_mapping_set(0xABCD, 0x0001, "Live Pad", &m));
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_START);

    /* Via JSON too. */
    const char *js = "{\"controllers\": [{\"id\": \"abcd:0001\", \"map\": {\"east\": \"y\"}}]}";
    CHECK(cm_mapping_parse(js, strlen(js)));
    CHECK(poll_input().pads[0].buttons == RETRO_BTN_Y);
    cm_device_disconnect(id);
}

static void test_mapping_serialize_round_trip(void)
{
    static char a[16384], b[16384];
    size_t la = cm_mapping_serialize(a, sizeof(a));
    CHECK(la > 0 && la == strlen(a));
    CHECK(strstr(a, "\"045e:0b12\"") != NULL);
    CHECK(strstr(a, "\"Xbox Series\"") != NULL);

    cm_mapping_t before, after;
    cm_mapping_get(0x045e, 0x0b12, &before);
    CHECK(cm_mapping_parse(a, la));
    cm_mapping_get(0x045e, 0x0b12, &after);
    CHECK(mapping_eq(&before, &after));
    size_t lb = cm_mapping_serialize(b, sizeof(b));
    CHECK(lb == la);
    CHECK_STR(b, a);

    /* Too small a buffer fails cleanly. */
    CHECK(cm_mapping_serialize(b, 64) == 0);
}

static void test_hotkey_combo_menu(void)
{
    fake_t f;
    int id = connect_pad(&f, "pad", 1, 2);
    press(id, CM_BIT(CM_BTN_SELECT) | CM_BIT(CM_BTN_START));
    CHECK(poll_input().hotkeys == 0);
    retro_sleep_ms(400);
    CHECK(poll_input().hotkeys == 0);
    retro_sleep_ms(700);
    retro_input_state_t in = poll_input();
    CHECK(in.hotkeys == RETRO_HOTKEY_MENU);
    CHECK(in.pads[0].buttons == (RETRO_BTN_SELECT | RETRO_BTN_START)); /* still passed on */
    /* Releasing one resets the timer. */
    press(id, CM_BIT(CM_BTN_START));
    CHECK(poll_input().hotkeys == 0);
    press(id, CM_BIT(CM_BTN_SELECT) | CM_BIT(CM_BTN_START));
    CHECK(poll_input().hotkeys == 0);
    press(id, 0);
    poll_input();
    cm_device_disconnect(id);
}

static void test_hotkey_combo_across_sources(void)
{
    /* SELECT on the keyboard, START on the pad: both reach player 1. */
    fake_t kb, pad;
    int k = connect_builtin(&kb);
    int p = connect_pad(&pad, "pad", 1, 2);
    press(k, CM_BIT(CM_BTN_SELECT));
    press(p, CM_BIT(CM_BTN_START));
    CHECK(poll_input().hotkeys == 0);
    retro_sleep_ms(1100);
    CHECK(poll_input().hotkeys == RETRO_HOTKEY_MENU);
    press(k, 0);
    CHECK(poll_input().hotkeys == 0);
    cm_device_disconnect(p);
    cm_device_disconnect(k);
}

static void test_hotkey_guide_power(void)
{
    fake_t f;
    int id = connect_pad(&f, "pad", 1, 2);
    press(id, CM_BIT(CM_BTN_GUIDE));
    CHECK(poll_input().hotkeys == RETRO_HOTKEY_MENU);
    retro_sleep_ms(1500);
    CHECK(poll_input().hotkeys == RETRO_HOTKEY_MENU);
    retro_sleep_ms(1600);
    CHECK(poll_input().hotkeys == (RETRO_HOTKEY_MENU | RETRO_HOTKEY_POWER));
    press(id, 0);
    CHECK(poll_input().hotkeys == 0);
    /* The hold timer restarts. */
    press(id, CM_BIT(CM_BTN_GUIDE));
    CHECK(poll_input().hotkeys == RETRO_HOTKEY_MENU);
    press(id, 0);
    poll_input();
    cm_device_disconnect(id);
}

int main(void)
{
    if (!cm_init()) {
        fprintf(stderr, "cm_init failed\n");
        return 1;
    }
    CHECK(cm_init()); /* idempotent */

    RUN(test_no_devices);
    RUN(test_players_in_connect_order);
    RUN(test_disconnect_reassigns_oldest);
    RUN(test_device_limit);
    RUN(test_builtin_feeds_player_one);
    RUN(test_default_positional_mapping);
    RUN(test_axes_copied);
    RUN(test_stick_dpad_hysteresis);
    RUN(test_poll_callback);
    RUN(test_rumble_dispatch);
    RUN(test_mapping_parse_applies_on_connect);
    RUN(test_mapping_parse_rejects);
    RUN(test_mapping_live_apply);
    RUN(test_mapping_serialize_round_trip);
    RUN(test_hotkey_combo_menu);
    RUN(test_hotkey_combo_across_sources);
    RUN(test_hotkey_guide_power);
    return TEST_EXIT();
}
