# Host unit tests (plain C, run through CTest). Included from host/CMakeLists.txt.
find_package(Threads REQUIRED)

function(retro_host_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

retro_host_test(test_blit retro_common)
retro_host_test(test_textring retro_common)
retro_host_test(test_log retro_hal)
retro_host_test(test_gfx retro_common)
retro_host_test(test_spsc retro_common Threads::Threads)
retro_host_test(test_crc32 retro_common)
retro_host_test(test_json retro_common)
retro_host_test(test_battery retro_common)
retro_host_test(test_storage retro_hal)

# Private headers of a component (vp_layout.h, ae_resampler.h, xpad_proto.h).
retro_host_test(test_resampler audio_engine m)
target_include_directories(test_resampler PRIVATE ${REPO_ROOT}/components/audio_engine)
retro_host_test(test_vp_layout video_pipeline)
target_include_directories(test_vp_layout PRIVATE ${REPO_ROOT}/components/video_pipeline)
retro_host_test(test_vp_mailbox video_pipeline Threads::Threads)
target_include_directories(test_vp_mailbox PRIVATE ${REPO_ROOT}/components/video_pipeline)
retro_host_test(test_xpad controller_manager)
target_include_directories(test_xpad PRIVATE ${REPO_ROOT}/components/controller_manager)
retro_host_test(test_controller_manager controller_manager)
