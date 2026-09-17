# Bring-up harness for firmware/pio/i2c_slave.pio.
#
# Scaffolding for tasks 12-18, not the product. It exists so the PIO program
# can be run against real silicon before the firmware project exists. The .pio
# itself lives in firmware/pio/ and is the deliverable; this only loads it.
#
#   cmake -S tests/slave -B tests/slave/build -G Ninja \
#     -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \
#     -Dpioasm_DIR=... -Dpicotool_DIR=...
#   cmake --build tests/slave/build

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(emuwire_i2c_slave_bringup C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The CPU half of the handshake is firmware source, not harness source. It is
# compiled from where it lives, so this harness tests the real file that the
# firmware project will pick up at task 25.
add_executable(i2c_slave_bringup
    main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/src/bus/i2c_bus.c)

target_include_directories(i2c_slave_bringup PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/src)

target_compile_options(i2c_slave_bringup PRIVATE -Wall -Wextra -Werror)

# Assemble the PIO program from its home in firmware/pio. Referenced rather
# than copied: one source of truth, and the instruction count CI reports is
# the same file this builds.
pico_generate_pio_header(i2c_slave_bringup
    ${CMAKE_CURRENT_LIST_DIR}/../../firmware/pio/i2c_slave.pio)

target_link_libraries(i2c_slave_bringup pico_stdlib hardware_pio pico_multicore)

pico_enable_stdio_usb(i2c_slave_bringup 1)
pico_enable_stdio_uart(i2c_slave_bringup 0)

pico_add_extra_outputs(i2c_slave_bringup)
