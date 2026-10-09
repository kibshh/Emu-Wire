/*
 * Bring-up harness for the PIO I2C slave.
 *
 * Core 0 owns USB and prints. Core 1 owns the bus: it services the handshake
 * IRQ, where the master is waiting in real time with SCL held low. That split
 * is the product's architecture, in miniature, and it is why nothing here
 * prints from the IRQ.
 *
 * One device is attached, at a BMP280's address, with enough of a BMP280's
 * register map to answer the master rig in tests/rig unchanged.
 *
 *   Wiring        slave GP4 <-> rig GP4  (SDA)
 *                 slave GP5 <-> rig GP5  (SCL)
 *                 GND <-> GND
 *   Pull-ups      1 kOhm from each line to 3V3, once, anywhere on the bus.
 *                 There is no sensor breakout here to provide them.
 *
 * Expected against the rig: the single-byte read, the chip-id read through a
 * repeated START, the absent-address NACK and the clock-stretch test all
 * pass. The burst read fails — only the first byte is sent — and the register
 * write fails, because the map here is read-only and the second byte of a
 * write is NACKed rather than quietly dropped.
 */

#include <stdio.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "bus/i2c_bus.h"

#include "i2c_slave.pio.h"

#define PIN_SDA 4 // SCL must be PIN_SDA + 1
#define BUS_HZ 400000

// The address this build answers to.
#define OUR_ADDR 0x76
#define OUR_DEVICE_ID 1

/*
 * Enough of a BMP280 to be recognised. The chip id is the real one; the
 * measurement registers hold fixed, made-up values, so a burst read has
 * something to return that is not all the same byte. The real device model,
 * with writable registers and manifests behind it, comes later.
 */
#define REG_CHIP_ID 0xD0
static const uint8_t bmp280_regs[256] = {
    [REG_CHIP_ID] = 0x58,                          // what a BMP280 answers
    [0xF7] = 0x51, [0xF8] = 0x2C, [0xF9] = 0x80,   // pressure  msb, lsb, xlsb
    [0xFA] = 0x82, [0xFB] = 0x3A, [0xFC] = 0x00,   // temperature
};

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

static void print_banner(void) {
    printf("\nEmuWire PIO I2C slave\n");
    printf("  program   %d of 32 PIO instructions, loaded at offset %u\n",
           i2c_slave_program.length, g_bus.offset);
    printf("  pins      SDA = GP%d, SCL = GP%d\n", PIN_SDA, PIN_SDA + 1);
    printf("  device    0x%02X, a BMP280 register map (0x%02X reads 0x%02X)\n", OUR_ADDR,
           REG_CHIP_ID, bmp280_regs[REG_CHIP_ID]);
    printf("  writes    refused: the map is read-only for now\n");
    printf("  core 1    serving the bus\n\n");
    printf("After each burst of traffic, a summary of what the slave did:\n\n");
}

// What the IRQ handler has counted so far. Copied in one go, so a summary is
// computed from one set of numbers rather than from counters still moving.
typedef struct {
    uint32_t acked, pointers, unknown, writes, stray, no_addr, blocked;
} counts_t;

static counts_t read_counts(void) {
    counts_t c = {g_bus.acked,      g_bus.pointer_writes, g_bus.nacked_unknown,
                  g_bus.nacked_write, g_bus.nacked_stray,  g_bus.no_address,
                  g_bus.tx_blocked};
    return c;
}

static uint32_t total(const counts_t *c) {
    return c->acked + c->pointers + c->unknown + c->writes + c->stray + c->no_addr + c->blocked;
}

// One burst of traffic, as the difference between two snapshots.
static void print_burst(const counts_t *now, const counts_t *before) {
    const uint8_t frame = g_bus.last_frame;
    const float t = (float)to_ms_since_boot(get_absolute_time()) / 1000.0f;

    printf("  [%7.1f s] answered %lu, register pointer set %lu times (now 0x%02X), "
           "last call 0x%02X %s\n",
           t, (unsigned long)(now->acked - before->acked),
           (unsigned long)(now->pointers - before->pointers), g_bus.devices[0].pointer,
           frame >> 1, (frame & 1u) ? "read" : "write");

    const uint32_t unknown = now->unknown - before->unknown;
    const uint32_t writes = now->writes - before->writes;
    if (unknown != 0 || writes != 0) {
        printf("              refused");
        if (unknown != 0) {
            printf(" %lu (unknown address)", (unsigned long)unknown);
        }
        if (writes != 0) {
            printf("%s %lu (write to the read-only map)", unknown != 0 ? "," : "",
                   (unsigned long)writes);
        }
        printf("\n");
    }

    // None of these can happen while the handshake behaves. Each means the
    // bus was held low, or is still being held.
    const uint32_t stray = now->stray - before->stray;
    const uint32_t no_addr = now->no_addr - before->no_addr;
    const uint32_t blocked = now->blocked - before->blocked;
    if (stray != 0) {
        printf("  !! PROBLEM: %lu data bytes arrived outside any transaction\n",
               (unsigned long)stray);
    }
    if (no_addr != 0) {
        printf("  !! PROBLEM: %lu handshakes with no byte to read\n", (unsigned long)no_addr);
    }
    if (blocked != 0) {
        printf("  !! PROBLEM: %lu handshakes with no room to answer: the bus may be stuck\n",
               (unsigned long)blocked);
    }
}

int main(void) {
    stdio_init_all();

    emuwire_status_t st = i2c_bus_init(&g_bus, pio0, 0, PIN_SDA, BUS_HZ);
    if (st == EMUWIRE_STATUS_OK) {
        st = i2c_bus_attach(&g_bus, OUR_ADDR, OUR_DEVICE_ID, bmp280_regs, sizeof bmp280_regs);
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

    print_banner();

    // Print once a burst of traffic has gone quiet, not on every change: a rig
    // run is one burst, and one summary per run is readable where a line per
    // counter change was not.
    const uint32_t QUIET_MS = 300;
    counts_t printed = read_counts();
    uint32_t last_seen = total(&printed);
    uint32_t last_change_ms = 0;
    bool was_connected = true;

    while (true) {
        const counts_t now = read_counts();
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        if (total(&now) != last_seen) {
            last_seen = total(&now);
            last_change_ms = now_ms;
        } else if (total(&now) != total(&printed) && now_ms - last_change_ms >= QUIET_MS) {
            print_burst(&now, &printed);
            printed = now;
        }

        // Reopening the serial port otherwise shows a blank screen, with no
        // way to tell a running slave from a dead one.
        const bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            print_banner();
        }
        was_connected = connected;

        sleep_ms(50);
    }
}
