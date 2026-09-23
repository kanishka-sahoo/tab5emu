idf_component_register(SRCS main.c pipeline_test.c
                       PRIV_REQUIRES retro_hal hwtest emulator_manager core_synth video_pipeline
                                     controller_manager esp_app_format esp_psram spi_flash
                                     app_update esp_driver_usb_serial_jtag)
