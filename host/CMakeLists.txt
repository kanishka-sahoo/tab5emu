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

add_subdirectory(${REPO_ROOT}/components/retro_common ${CMAKE_BINARY_DIR}/retro_common)
add_subdirectory(${REPO_ROOT}/components/retro_hal ${CMAKE_BINARY_DIR}/retro_hal)

add_executable(tab5emu_host src/host_main.c)
target_link_libraries(tab5emu_host PRIVATE retro_hal m)

enable_testing()
add_subdirectory(${REPO_ROOT}/test/host ${CMAKE_BINARY_DIR}/test)
