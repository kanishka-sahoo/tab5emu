# Hardware bring-up tests (plan Phase 1), later Recovery -> "Test hardware".
# Uses the BSP only through retro_hal's Tab5 board layer (retro_tab5.h).
idf_component_register(SRCS hwtest.c
                            hwtest_gfx.c
                            hwtest_board.c
                            hwtest_display.c
                            hwtest_sd.c
                            hwtest_audio.c
                            hwtest_touch.c
                            hwtest_usb.c
                            hwtest_power.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES retro_hal retro_common esp_app_format esp_driver_gpio
                                     esp_driver_ppa esp_driver_usb_serial_jtag esp_hw_support
                                     esp_mm esp_psram esp_timer nvs_flash)
