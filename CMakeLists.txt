cmake_minimum_required(VERSION 3.16)
project(xnet VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "xnet requires a Linux target (including WSL).")
endif()

include(GNUInstallDirs)
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
target_include_directories(xnet PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(xnet PUBLIC Threads::Threads)

set(xnet_install_default OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(xnet_install_default ON)
endif()
option(XNET_INSTALL "Install xnet headers, library and CMake package" "${xnet_install_default}")

if(XNET_INSTALL)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        message(FATAL_ERROR "xnet requires relative install directories for a relocatable package.")
    endif()
    include(CMakePackageConfigHelpers)
    set(xnet_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/xnet")
    install(TARGETS xnet EXPORT xnetTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY include/xnet DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT xnetTargets NAMESPACE xnet::
        DESTINATION "${xnet_package_dir}")

    configure_package_config_file(
        cmake/xnetConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/xnetConfig.cmake"
        INSTALL_DESTINATION "${xnet_package_dir}"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/xnetConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/xnetConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/xnetConfigVersion.cmake"
        DESTINATION "${xnet_package_dir}"
    )
endif()

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

    if(XNET_INSTALL)
        find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME xnet_installed_package
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_installed_package.py"
                    --cmake "${CMAKE_COMMAND}"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --build "${CMAKE_CURRENT_BINARY_DIR}"
                    --generator "${CMAKE_GENERATOR}"
                    "--config=$<CONFIG>")
            set_tests_properties(xnet_installed_package PROPERTIES TIMEOUT 90)
        endif()
    endif()
endif()
