# Synthetic test core (plan Phase 2 exit test). Like every core it may only
# use RetroHAL headers and retro_common (plan §3.1).
if(ESP_PLATFORM)
    idf_component_register(SRCS core_synth.c
                           INCLUDE_DIRS include
                           REQUIRES retro_hal retro_common)
else()
    add_library(core_synth STATIC core_synth.c)
    target_include_directories(core_synth PUBLIC include)
    target_link_libraries(core_synth PUBLIC retro_hal retro_common PRIVATE m)
endif()
