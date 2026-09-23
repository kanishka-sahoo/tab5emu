# Emulator manager: frontend bring-up, frame loop, statistics (plan Phase 2).
set(EMU_SRCS
    emu_monitor.c
    emu_runner.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${EMU_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES retro_hal
                           PRIV_REQUIRES retro_common video_pipeline audio_engine controller_manager)
else()
    add_library(emulator_manager STATIC ${EMU_SRCS})
    target_include_directories(emulator_manager PUBLIC include PRIVATE .)
    target_link_libraries(emulator_manager PUBLIC retro_hal
                          PRIVATE retro_common video_pipeline audio_engine controller_manager m)
endif()
