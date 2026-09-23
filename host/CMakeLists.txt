# Desktop build (plan decision D8): platform-neutral components + the SDL2
# RetroHAL backend, and host unit tests. Not an ESP-IDF project.
#
#   cmake -S host -B build-host -G Ninja
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(tab5emu_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(RETRO_WERROR "Treat warnings as errors" OFF)
add_compile_options(-Wall -Wextra)
if(RETRO_WERROR)
    add_compile_options(-Werror)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

foreach(c retro_common retro_hal video_pipeline audio_engine controller_manager emulator_manager
          core_synth)
    add_subdirectory(${REPO_ROOT}/components/${c} ${CMAKE_BINARY_DIR}/${c})
endforeach()

add_executable(tab5emu_host src/host_main.c)
target_link_libraries(tab5emu_host PRIVATE emulator_manager core_synth video_pipeline audio_engine
                      controller_manager retro_hal m)

enable_testing()
add_subdirectory(${REPO_ROOT}/test/host ${CMAKE_BINARY_DIR}/test)
