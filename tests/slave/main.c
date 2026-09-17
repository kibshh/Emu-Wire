/*
 * Bring-up harness for the PIO I2C slave and the clock-stretch ACK handshake.
 *
 * Core 0 owns USB and prints. Core 1 owns the bus: it services the handshake
 * IRQ, where the master is waiting in real time with SCL held low. That split
 * is the product's architecture, in miniature, and it is why nothing here
 * prints from the IRQ.
 *
 * One device is attached, at a BMP280's address, returning a BMP280's chip id,
 * so the master rig in tests/rig can be pointed at it unchanged.
 *
 *   Wiring        slave GP4 <-> rig GP4  (SDA)
 *                 slave GP5 <-> rig GP5  (SCL)
 *                 GND <-> GND
 *   Pull-ups      1 kOhm from each line to 3V3, once, anywhere on the bus.
 *                 There is no sensor breakout here to provide them.
 *
 * Expected against the rig: the single-byte read passes and reads 0x58, the
 * absent-address test passes because unknown addresses are now NACKed, and
 * the clock-stretch test reports a real measured stretch. The write-based
 * tests fail: there is no write path yet, so writes are NACKed on purpose.
 */

#include <stdio.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "bus/i2c_bus.h"

#include "i2c_slave.pio.h"

#define PIN_SDA 4 // SCL must be PIN_SDA + 1
#define BUS_HZ 400000

// The address this build answers to, and the byte it returns.
#define OUR_ADDR 0x76
#define OUR_BYTE 0x58
#define OUR_DEVICE_ID 1

static i2c_bus_t g_bus;

// Core 1 cannot print: USB belongs to core 0. It leaves its result here.
static volatile int32_t g_core1_status = -1;

static void core1_main(void) {
    // Interrupt enables are per core, so the core that calls this is the core
    // that serves the bus. That is the whole reason this runs here.
    g_core1_status = (int32_t)i2c_bus_start(&g_bus);

    while (true) {
        __wfi();
    }
}

static void print_banner(uint offset) {
    printf("\nEmuWire PIO I2C slave — clock-stretch ACK handshake\n");
    printf("  program:  %d of 32 instructions, loaded at offset %u\n", i2c_slave_program.length,
           offset);
    printf("  pins:     SDA GP%d, SCL GP%d\n", PIN_SDA, PIN_SDA + 1);
    printf("  answers:  0x%02X, returns 0x%02X\n", OUR_ADDR, OUR_BYTE);
    printf("  core 1:   serving the handshake IRQ\n");
    printf("  NOTE:     reads are ACKed, writes are NACKed — no write path yet.\n\n");
}

int main(void) {
    stdio_init_all();

    emuwire_status_t st = i2c_bus_init(&g_bus, pio0, 0, PIN_SDA, BUS_HZ);
    if (st == EMUWIRE_STATUS_OK) {
        st = i2c_bus_attach(&g_bus, OUR_ADDR, OUR_DEVICE_ID, OUR_BYTE);
    }

    // Only start the bus if it is set up. A state machine that stretches with
    // nobody answering holds SCL low for ever, which looks like dead hardware.
    if (st == EMUWIRE_STATUS_OK) {
        multicore_launch_core1(core1_main);
    }

    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }

    if (st != EMUWIRE_STATUS_OK) {
        printf("\nBus setup FAILED with status 0x%02X. The bus is not running.\n", (unsigned)st);
        while (true) {
            sleep_ms(1000);
        }
    }

    while (g_core1_status < 0) {
        sleep_ms(1);
    }
    if (g_core1_status != (int32_t)EMUWIRE_STATUS_OK) {
        printf("\nCore 1 could not start the bus: status 0x%02X\n", (unsigned)g_core1_status);
        while (true) {
            sleep_ms(1000);
        }
    }

    print_banner(g_bus.offset);

    uint32_t last_total = 0;
    bool was_connected = true;

    while (true) {
        const uint32_t acked = g_bus.acked;
        const uint32_t unknown = g_bus.nacked_unknown;
        const uint32_t writes = g_bus.nacked_write;
        const uint32_t no_addr = g_bus.no_address;
        const uint32_t blocked = g_bus.tx_blocked;
        const uint8_t frame = g_bus.last_frame;
        const uint32_t total = acked + unknown + writes + no_addr + blocked;

        if (total != last_total) {
            last_total = total;
            printf("  acked %lu   NACKed: unknown addr %lu, write %lu   last 0x%02X "
                   "(addr 0x%02X %s)\n",
                   (unsigned long)acked, (unsigned long)unknown, (unsigned long)writes, frame,
                   frame >> 1, (frame & 1u) ? "read" : "write");

            // Neither can happen while the handshake behaves. Both mean the
            // bus was, or still is, held low.
            if (no_addr != 0 || blocked != 0) {
                printf("  PROBLEM: %lu IRQs with no address, %lu with no room to answer\n",
                       (unsigned long)no_addr, (unsigned long)blocked);
            }
        }

        // Reopening the serial port otherwise shows a blank screen, with no
        // way to tell a running slave from a dead one.
        const bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            print_banner(g_bus.offset);
            last_total = 0;
        }
        was_connected = connected;

        sleep_ms(100);
    }
}
