cmake_minimum_required(VERSION 3.13)

# Requires PICO_SDK_PATH to point at a pico-sdk checkout (with tinyusb submodule)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(usb_gamepad C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(usb_gamepad
    main.c
    usb_descriptors.c
)

target_include_directories(usb_gamepad PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(usb_gamepad PRIVATE
    pico_stdlib
    hardware_adc
    tinyusb_device
    tinyusb_board
)

pico_add_extra_outputs(usb_gamepad)
