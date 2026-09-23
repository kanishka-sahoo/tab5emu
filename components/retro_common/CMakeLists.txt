# Platform-neutral helpers. Must not depend on ESP-IDF or SDL so that cores
# and the host build can use it.
set(RETRO_COMMON_SRCS
    retro_blit.c
    retro_textring.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RETRO_COMMON_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(retro_common STATIC ${RETRO_COMMON_SRCS})
    target_include_directories(retro_common PUBLIC include)
endif()
