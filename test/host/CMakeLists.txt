# Host unit tests (plain C, run through CTest). Included from host/CMakeLists.txt.
function(retro_host_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

retro_host_test(test_textring retro_common)
retro_host_test(test_log retro_hal)
