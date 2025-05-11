cmake_minimum_required(VERSION 3.31)
project(gamepad_driver C)

set(CMAKE_C_STANDARD 23)

add_executable(gamepad_driver main.c)

find_package(PkgConfig REQUIRED)
pkg_search_module(LIBUSB1 REQUIRED libusb-1.0)
include_directories(${LIBUSB1_INCLUDE_DIRS})
target_link_libraries(gamepad_driver ${LIBUSB1_LIBRARIES})
add_definitions(${LIBUSB1_CFLAGS_OTHER})