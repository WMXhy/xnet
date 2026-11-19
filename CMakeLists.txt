cmake_minimum_required(VERSION 3.16)
project(xnet LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "xnet requires a Linux target (including WSL).")
endif()

find_package(Threads REQUIRED)

add_library(xnet STATIC
    src/acceptor.cpp
    src/connection.cpp
    src/event_loop.cpp
    src/network_buffer.cpp
    src/tcp_server.cpp
)
add_library(xnet::xnet ALIAS xnet)
set_target_properties(xnet PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(xnet PUBLIC cxx_std_17)
target_include_directories(xnet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xnet PUBLIC Threads::Threads)

set(xnet_build_tests_default OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(CTest)
    set(xnet_build_tests_default "${BUILD_TESTING}")
endif()
option(XNET_BUILD_TESTS "Build xnet tests" "${xnet_build_tests_default}")

if(XNET_BUILD_TESTS)
    enable_testing()
    add_executable(xnet_loopback_test
        tests/loopback.cpp
        tests/public_headers.cpp
    )
    target_link_libraries(xnet_loopback_test PRIVATE xnet::xnet)
    add_test(NAME xnet_loopback COMMAND xnet_loopback_test)
    add_test(NAME xnet_quiesce COMMAND xnet_loopback_test quiesce)
    set_tests_properties(xnet_loopback xnet_quiesce PROPERTIES TIMEOUT 15)

    add_executable(xnet_transport_test tests/transport.cpp)
    target_link_libraries(xnet_transport_test PRIVATE xnet::xnet)
    foreach(test_case IN ITEMS short_write slow_reader queue_full startup_failure concurrent_shutdown drain_timeout)
        add_test(NAME xnet_${test_case} COMMAND xnet_transport_test ${test_case})
        set_tests_properties(xnet_${test_case} PROPERTIES TIMEOUT 15)
    endforeach()
endif()
