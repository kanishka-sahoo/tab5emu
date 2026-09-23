# Controller manager: input sources -> four logical players (plan Phase 2).
# Everything but the USB class driver (xinput_host.c) is platform-neutral.
set(CM_SRCS
    cm_core.c
    cm_mapping.c
    cm_sources.c
    xpad_proto.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${CM_SRCS} xinput_host.c
                           INCLUDE_DIRS include
                           REQUIRES retro_hal retro_common
                           PRIV_REQUIRES usb)
else()
    add_library(controller_manager STATIC ${CM_SRCS})
    target_include_directories(controller_manager PUBLIC include PRIVATE .)
    target_link_libraries(controller_manager PUBLIC retro_hal retro_common)
endif()
