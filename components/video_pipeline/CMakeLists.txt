# Video pipeline: frame mailbox, display modes, scaler selection, video_out
# task (plan Phase 2). Platform-neutral: runs on the HAL, so the host build
# uses it too.
set(VP_SRCS
    video_pipeline.c
    vp_layout.c
    vp_mailbox.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${VP_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES retro_hal retro_common)
else()
    add_library(video_pipeline STATIC ${VP_SRCS})
    target_include_directories(video_pipeline PUBLIC include PRIVATE .)
    target_link_libraries(video_pipeline PUBLIC retro_hal retro_common)
endif()
