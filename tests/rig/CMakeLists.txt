# I2C master test rig — a separate program for the second Pico.
#
# Not part of the emulator firmware. This is test equipment: it drives the bus
# and judges what answers, so it is built and flashed on its own.
#
#   cmake -S tests/rig -B tests/rig/build -G Ninja \
#     -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350 \
#     -Dpioasm_DIR="$HOME/.pico-sdk/tools/2.3.0/pioasm" \
#     -Dpicotool_DIR="$HOME/.pico-sdk/picotool/2.3.0/picotool"
#   cmake --build tests/rig/build

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(emuwire_i2c_rig C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(i2c_rig main.c)

# This is ground truth, so it is built with warnings as errors. A warning here
# is a result nobody should trust.
target_compile_options(i2c_rig PRIVATE -Wall -Wextra -Werror)

# hardware_i2c, not PIO. The rig is ground truth, so it uses the silicon I2C
# controller rather than sharing an implementation with the thing it tests.
target_link_libraries(i2c_rig pico_stdlib hardware_i2c)

# Report over USB. UART is left off so the rig needs only the one cable, and
# so it cannot be confused with the emulator's own USB CDC link.
pico_enable_stdio_usb(i2c_rig 1)
pico_enable_stdio_uart(i2c_rig 0)

# .uf2 plus map/bin/hex
pico_add_extra_outputs(i2c_rig)
