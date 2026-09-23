/*
 * Tab5 board: I2C bus, IO expanders, rails, panel revision.
 *
 * Expander pin map from plan §2a.2 and M5Unified's Power_Class (Tab5 case).
 * M5Unified switches EXT 5 V (and USB-A 5 V) through the pin's pull resistor
 * rather than driving it. EXT 5 V is kept that way here; USB-A 5 V is driven
 * push-pull, which is what the BSP's USB feature does anyway.
 *
 * The expander driver refuses to set the level of a pin that is still an
 * input, so outputs are switched to output mode before their level is set.
 */
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "retro_log.h"
#include "retro_tab5.h"

#define ST712X_TOUCH_ADDR 0x55
#define GT911_TOUCH_ADDR 0x14

typedef struct {
    const char *name;
    bool second_expander; /* false: 0x43, true: 0x44 */
    uint8_t pin;
    bool pull_switched; /* EXT5V_EN: switched via pull-up/down, never driven */
    bool inverted;      /* nCHG_QC_EN */
} rail_desc_t;

static const rail_desc_t s_rails[RETRO_TAB5_RAIL_COUNT] = {
    [RETRO_TAB5_RAIL_WLAN] = {"wlan", true, 0, false, false},
    [RETRO_TAB5_RAIL_USB_A] = {"usb_a_5v", true, 3, false, false},
    [RETRO_TAB5_RAIL_EXT_5V] = {"ext_5v", false, 2, true, false},
    [RETRO_TAB5_RAIL_SPEAKER] = {"speaker", false, 1, false, false},
    [RETRO_TAB5_RAIL_CHARGE] = {"charge", true, 7, false, false},
    [RETRO_TAB5_RAIL_QUICK_CHARGE] = {"quick_charge", true, 5, false, true},
};

#define EXP0_PIN_ANTENNA 0
#define EXP0_PIN_HP_DET 7
#define EXP1_PIN_PWROFF 4
#define EXP1_PIN_CHG_STAT 6

static esp_io_expander_handle_t s_exp[2];
static retro_tab5_panel_t s_panel = RETRO_TAB5_PANEL_UNKNOWN;
static bool s_board_ok;

static bool set_output(esp_io_expander_handle_t exp, uint8_t pin, bool level)
{
    uint32_t mask = 1u << pin;
    esp_err_t err = esp_io_expander_set_pullupdown(exp, mask, IO_EXPANDER_PULL_NONE);
    err |= esp_io_expander_set_dir(exp, mask, IO_EXPANDER_OUTPUT);
    err |= esp_io_expander_set_output_mode(exp, mask, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    err |= esp_io_expander_set_level(exp, mask, level);
    return err == ESP_OK;
}

static bool set_input(esp_io_expander_handle_t exp, uint8_t pin, esp_io_expander_pullupdown_t pull)
{
    uint32_t mask = 1u << pin;
    esp_err_t err = esp_io_expander_set_dir(exp, mask, IO_EXPANDER_INPUT);
    err |= esp_io_expander_set_pullupdown(exp, mask, pull);
    return err == ESP_OK;
}

static int read_pin(esp_io_expander_handle_t exp, uint8_t pin)
{
    uint32_t levels = 0;
    if (!exp || esp_io_expander_get_level(exp, 1u << pin, &levels) != ESP_OK) {
        return -1;
    }
    return levels ? 1 : 0;
}

bool retro_tab5_rail_set(retro_tab5_rail_t rail, bool on)
{
    if ((unsigned)rail >= RETRO_TAB5_RAIL_COUNT) {
        return false;
    }
    const rail_desc_t *r = &s_rails[rail];
    esp_io_expander_handle_t exp = s_exp[r->second_expander];
    if (!exp) {
        return false;
    }
    bool level = r->inverted ? !on : on;
    bool ok;
    if (r->pull_switched) {
        ok = esp_io_expander_set_pullupdown(exp, 1u << r->pin,
                                            level ? IO_EXPANDER_PULL_UP : IO_EXPANDER_PULL_DOWN) ==
             ESP_OK;
    } else {
        ok = set_output(exp, r->pin, level);
    }
    RLOGD(POWER, "rail %s -> %s%s", r->name, on ? "on" : "off", ok ? "" : " FAILED");
    return ok;
}

int retro_tab5_rail_get(retro_tab5_rail_t rail)
{
    if ((unsigned)rail >= RETRO_TAB5_RAIL_COUNT) {
        return -1;
    }
    /* The PI4IOE5V6408's input status reads 0 on output pins, so outputs
     * report their OUT_SET bit and the pull-switched rail its input level. */
    const rail_desc_t *r = &s_rails[rail];
    uint8_t regs[6];
    if (!retro_tab5_expander_regs(r->second_expander, regs)) {
        return -1;
    }
    int v = (regs[r->pull_switched ? 5 : 1] >> r->pin) & 1;
    return r->inverted ? !v : v;
}

const char *retro_tab5_rail_name(retro_tab5_rail_t rail)
{
    return (unsigned)rail < RETRO_TAB5_RAIL_COUNT ? s_rails[rail].name : "?";
}

int retro_tab5_headphones(void)
{
    return read_pin(s_exp[0], EXP0_PIN_HP_DET);
}

bool retro_tab5_expander_regs(int index, uint8_t regs[6])
{
    static const uint8_t reg_addr[6] = {0x03, 0x05, 0x07, 0x0B, 0x0D, 0x0F};
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = index ? BSP_IO_EXPANDER_ADDRESS_1 : BSP_IO_EXPANDER_ADDRESS,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (bsp_i2c_init() != ESP_OK ||
        i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < 6 && ok; i++) {
        ok = i2c_master_transmit_receive(dev, &reg_addr[i], 1, &regs[i], 1, 50) == ESP_OK;
    }
    i2c_master_bus_rm_device(dev);
    return ok;
}

int retro_tab5_charge_stat_pin(void)
{
    return read_pin(s_exp[1], EXP1_PIN_CHG_STAT);
}

void retro_tab5_power_off(void)
{
    /* Same sequence as M5Unified: toggle PWROFF_PLUSE 10x at 50 ms. */
    RLOGW(POWER, "requesting power-off (PWROFF_PLUSE pulse train)");
    for (int i = 0; i < 10; i++) {
        set_output(s_exp[1], EXP1_PIN_PWROFF, i & 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    set_output(s_exp[1], EXP1_PIN_PWROFF, false);
    RLOGW(POWER, "still powered after the power-off request");
}

/* Mirrors bsp_get_board_version() (BSP 1.3.1, bsp_display.c), which is
 * private: the ST712x touch controller answers at 0x55 and reports its
 * firmware version at register 0x0000 (1 = ST7121, 3 = ST7123); the GT911
 * answers at 0x14. The touch controller must be powered (TP_RST released). */
static retro_tab5_panel_t detect_panel(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, ST712X_TOUCH_ADDR, 100) == ESP_OK) {
        i2c_master_dev_handle_t dev;
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ST712X_TOUCH_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
            return RETRO_TAB5_PANEL_UNKNOWN;
        }
        const uint8_t reg[2] = {0x00, 0x00};
        uint8_t fw = 0;
        esp_err_t err = i2c_master_transmit_receive(dev, reg, sizeof(reg), &fw, 1, 100);
        i2c_master_bus_rm_device(dev);
        if (err != ESP_OK) {
            return RETRO_TAB5_PANEL_UNKNOWN;
        }
        RLOGI(VIDEO, "ST712x touch firmware %u", fw);
        if (fw == 1) {
            return RETRO_TAB5_PANEL_ST7121;
        }
        if (fw == 3) {
            return RETRO_TAB5_PANEL_ST7123;
        }
        return RETRO_TAB5_PANEL_UNKNOWN;
    }
    if (i2c_master_probe(bus, GT911_TOUCH_ADDR, 100) == ESP_OK) {
        return RETRO_TAB5_PANEL_ILI9881C_GT911;
    }
    return RETRO_TAB5_PANEL_UNKNOWN;
}

bool retro_tab5_board_init(void)
{
    if (s_board_ok) {
        return true;
    }
    if (bsp_i2c_init() != ESP_OK) {
        RLOGE(CORE, "I2C init failed");
        return false;
    }
    /* The chip reset in the expander driver occasionally NAKs right after
     * power-on; the BSP keeps the handle NULL then, so just ask again. */
    for (int attempt = 0; attempt < 3 && (!s_exp[0] || !s_exp[1]); attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        s_exp[0] = bsp_io_expander_init();
        s_exp[1] = bsp_io_expander1_init();
    }
    if (!s_exp[0] || !s_exp[1]) {
        RLOGE(CORE, "IO expander init failed (0x43 %s, 0x44 %s)", s_exp[0] ? "ok" : "missing",
              s_exp[1] ? "ok" : "missing");
        return false;
    }

    /* 0x43: antenna internal, speaker amp off, EXT 5 V on, HP_DET input.
     * LCD/touch/camera reset pins are left to the BSP's feature switches. */
    bool ok = set_output(s_exp[0], EXP0_PIN_ANTENNA, false);
    ok &= set_input(s_exp[0], EXP0_PIN_HP_DET, IO_EXPANDER_PULL_NONE);
    /* 0x44: CHG_STAT input, PWROFF_PLUSE idle low. */
    ok &= set_input(s_exp[1], EXP1_PIN_CHG_STAT, IO_EXPANDER_PULL_NONE);
    ok &= set_output(s_exp[1], EXP1_PIN_PWROFF, false);
    /* EXT 5 V stays an input, like M5Unified leaves it. */
    ok &= set_input(s_exp[0], s_rails[RETRO_TAB5_RAIL_EXT_5V].pin, IO_EXPANDER_PULL_UP);
    ok &= retro_tab5_rail_set(RETRO_TAB5_RAIL_USB_A, true);
    ok &= retro_tab5_rail_set(RETRO_TAB5_RAIL_SPEAKER, false);
    ok &= retro_tab5_rail_set(RETRO_TAB5_RAIL_CHARGE, true);
    ok &= retro_tab5_rail_set(RETRO_TAB5_RAIL_QUICK_CHARGE, true);
    ok &= retro_tab5_rail_set(RETRO_TAB5_RAIL_WLAN, false);
    if (!ok) {
        RLOGW(POWER, "some IO expander writes failed");
    }

    /* LCD and touch must be out of reset before the touch controller can be
     * probed (the ST712x is a TDDI: touch lives in the display chip). The
     * BSP waits 500 ms here too, and repeats this during display init. */
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    s_panel = detect_panel(bsp_i2c_get_handle());
    RLOGI(CORE, "board: Tab5, panel %s", retro_tab5_panel_name(s_panel));

    s_board_ok = true;
    return true;
}

retro_tab5_panel_t retro_tab5_panel(void)
{
    return s_panel;
}

const char *retro_tab5_panel_name(retro_tab5_panel_t panel)
{
    switch (panel) {
    case RETRO_TAB5_PANEL_ILI9881C_GT911: return "ILI9881C + GT911";
    case RETRO_TAB5_PANEL_ST7123: return "ST7123";
    case RETRO_TAB5_PANEL_ST7121: return "ST7121";
    default: return "unknown";
    }
}

int retro_tab5_i2c_scan(uint8_t *addrs, int max)
{
    if (bsp_i2c_init() != ESP_OK) {
        return 0;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            if (n < max) {
                addrs[n] = a;
            }
            n++;
        }
    }
    return n;
}
