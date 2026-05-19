cmake_minimum_required(VERSION 3.15)

project(uartbinlib C)

add_library(uartbin STATIC src/uartbin.c)
target_include_directories(uartbin PUBLIC include)
target_compile_features(uartbin PUBLIC c_std_99)

option(UARTBIN_BUILD_TESTS "Build uartbin host tests" ON)

if(UARTBIN_BUILD_TESTS)
    enable_testing()
    add_executable(uartbin_tests tests/test_uartbin.c)
    target_link_libraries(uartbin_tests PRIVATE uartbin)
    add_test(NAME uartbin_tests COMMAND uartbin_tests)
endif()
