cmake_minimum_required(VERSION 3.16)
project(xnet_installed_examples LANGUAGES CXX)

find_package(xnet 0.1 CONFIG REQUIRED)

add_executable(xnet_echo echo.cpp)
add_executable(xnet_line_echo line_echo.cpp)
target_link_libraries(xnet_echo PRIVATE xnet::xnet)
target_link_libraries(xnet_line_echo PRIVATE xnet::xnet)
