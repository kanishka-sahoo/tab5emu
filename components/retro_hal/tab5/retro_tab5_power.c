/*
 * Tab5 battery monitor: INA226 at 0x41 on the internal bus, 5 mOhm shunt on
 * the 2S pack (values from M5Unified's Tab5 setup). The shunt is wired so
 * that charge current reads negative; the sign is flipped here so that
 * + means charging.
 *
 * Current is computed from the shunt voltage register directly, so the
 * calibration register is never used.
 */
#include "bsp/m5stack_tab5.h"
#include "sdkconfig.h"

#include "retro_log.h"
#include "retro_tab5.h"

#define INA226_ADDR 0x41
#define REG_CONFIG 0x00
#define REG_SHUNT 0x01
#define REG_BUS 0x02
#define REG_MANUF_ID 0xFE
#define INA226_MANUF_TI 0x5449

/* CONFIG: AVG[11:9], VBUSCT[8:6], VSHCT[5:3], MODE[2:0].
 * 1.1 ms conversions, continuous shunt + bus. */
#define CONFIG_CT_1100US ((4u << 6) | (4u << 3))
#define CONFIG_MODE_CONT 7u

static i2c_master_dev_handle_t s_dev;

static bool write16(uint8_t reg, uint16_t v)
{
    const uint8_t buf[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50) == ESP_OK;
}

static bool read16(uint8_t reg, uint16_t *v)
{
    uint8_t buf[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 50) != ESP_OK) {
        return false;
    }
    *v = (uint16_t)((buf[0] << 8) | buf[1]);
    return true;
}

static bool ina226_init(void)
{
    if (s_dev) {
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    uint16_t id = 0;
    if (!read16(REG_MANUF_ID, &id) || id != INA226_MANUF_TI) {
        RLOGE(POWER, "INA226 not found at 0x%02x (id 0x%04x)", INA226_ADDR, id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    return retro_tab5_battery_set_averaging(16);
}

bool retro_tab5_battery_set_averaging(unsigned samples)
{
    if (!s_dev && !ina226_init()) {
        return false;
    }
    static const unsigned steps[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    unsigned code = 0;
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (samples >= steps[i]) {
            code = i;
        }
    }
    return write16(REG_CONFIG, (uint16_t)((code << 9) | CONFIG_CT_1100US | CONFIG_MODE_CONT));
}

bool retro_tab5_battery_read(retro_tab5_power_sample_t *out)
{
    if (!ina226_init()) {
        return false;
    }
    uint16_t shunt_raw, bus_raw;
    if (!read16(REG_SHUNT, &shunt_raw) || !read16(REG_BUS, &bus_raw)) {
        return false;
    }
    /* Shunt LSB 2.5 uV, bus LSB 1.25 mV. */
    int64_t shunt_nv = (int64_t)(int16_t)shunt_raw * 2500;
    out->bus_mv = (int32_t)((uint32_t)bus_raw * 125 / 100);
    out->current_ma = (int32_t)(-shunt_nv / (CONFIG_RETRO_TAB5_SHUNT_MOHM * 1000));
    out->power_mw = (int32_t)((int64_t)out->bus_mv * out->current_ma / 1000);
    return true;
}
