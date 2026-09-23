# Audio engine: SPSC ring, DRC resampler, audio_out feeder task (plan D3,
# Phase 2). Platform-neutral.
set(AE_SRCS
    ae_resampler.c
    audio_engine.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${AE_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES retro_hal retro_common)
else()
    add_library(audio_engine STATIC ${AE_SRCS})
    target_include_directories(audio_engine PUBLIC include PRIVATE .)
    target_link_libraries(audio_engine PUBLIC retro_hal retro_common)
endif()
