# RetroHAL: public platform headers (include/) with one backend per platform.
#   tab5/  ESP-IDF + m5stack_tab5 BSP. The only code allowed to touch the BSP.
#   host/  SDL2, for the desktop build (host/CMakeLists.txt).
set(RETRO_HAL_COMMON_SRCS
    src/retro_log.c
)

if(ESP_PLATFORM)
    # tab5/include/retro_tab5.h is the Tab5 board layer (firmware only). It
    # has no IDF types, so exposing it doesn't leak driver headers.
    idf_component_register(SRCS ${RETRO_HAL_COMMON_SRCS}
                                tab5/retro_log_tab5.c
                                tab5/retro_time_tab5.c
                                tab5/retro_tab5_board.c
                                tab5/retro_tab5_display.c
                                tab5/retro_tab5_touch.c
                                tab5/retro_tab5_audio.c
                                tab5/retro_tab5_sd.c
                                tab5/retro_tab5_usb.c
                                tab5/retro_tab5_power.c
                           INCLUDE_DIRS include tab5/include
                           PRIV_INCLUDE_DIRS src
                           REQUIRES retro_common
                           PRIV_REQUIRES log esp_timer esp_mm esp_lcd esp_driver_i2c esp_driver_i2s
                                         sdmmc fatfs)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)

    add_library(retro_hal STATIC
        ${RETRO_HAL_COMMON_SRCS}
        host/retro_log_host.c
        host/retro_time_host.c
        host/retro_platform_sdl.c
        host/retro_video_sdl.c
        host/retro_audio_sdl.c
    )
    target_include_directories(retro_hal PUBLIC include PRIVATE src)
    target_link_libraries(retro_hal PUBLIC retro_common PRIVATE SDL2::SDL2 Threads::Threads)
endif()
