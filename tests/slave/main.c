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

#include "hardware/sync.h"
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
    // Markers either side of the end of the map, not BMP280 values: a burst
    // from 0xFE shows whether the pointer wraps (E0 E1 E2 E3) or saturates
    // on the last register (E0 E1 E1 E1).
    [0xFE] = 0xE0, [0xFF] = 0xE1, [0x00] = 0xE2, [0x01] = 0xE3,
};

static const char *mode_name(uint8_t mode) {
    switch (mode) {
    case EMUWIRE_AUTO_INCREMENT_NONE:
        return "none";
    case EMUWIRE_AUTO_INCREMENT_ON_READ:
        return "on read";
    case EMUWIRE_AUTO_INCREMENT_ON_WRITE:
        return "on write";
    default:
        return "on read and write";
    }
}

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

// What a 4-byte burst from 0xFE should return under the current settings.
static void print_pointer_mode(void) {
    const i2c_device_t *dev = &g_bus.devices[0];
    const bool wrap = (dev->flags & EMUWIRE_DEVICE_FLAGS_WRAP) != 0;
    const bool moves = dev->auto_increment == EMUWIRE_AUTO_INCREMENT_ON_READ ||
                       dev->auto_increment == EMUWIRE_AUTO_INCREMENT_BOTH;
    printf("  pointer   auto-increment %s, %s at the end of the map\n",
           mode_name(dev->auto_increment), wrap ? "wraps to 0x00" : "stays on 0xFF");
    printf("            so a burst from 0xFE reads %s\n",
           !moves ? "E0 E0 E0 E0" : wrap ? "E0 E1 E2 E3" : "E0 E1 E1 E1");
}

static void print_banner(void) {
    printf("\nEmuWire PIO I2C slave\n");
    printf("  program   %d of 32 PIO instructions, loaded at offset %u\n",
           i2c_slave_program.length, g_bus.offset);
    printf("  pins      SDA = GP%d, SCL = GP%d\n", PIN_SDA, PIN_SDA + 1);
    printf("  device    0x%02X, a BMP280 register map (0x%02X reads 0x%02X)\n", OUR_ADDR,
           REG_CHIP_ID, bmp280_regs[REG_CHIP_ID]);
    printf("  writes    refused: the map is read-only for now\n");
    print_pointer_mode();
    printf("  keys      a = next auto-increment mode, w = toggle wrap\n");
    printf("  core 1    serving the bus\n");
#if I2C_BUS_VERBOSE
    printf("  log       every call on the bus, as it happens (verbose build)\n\n");
#else
    printf("  log       failures only (quiet build: I2C_BUS_VERBOSE=OFF)\n\n");
#endif
}

#if I2C_BUS_VERBOSE
// One line per log entry. The data bytes of a call are indented under its
// address line, so a register write reads top to bottom.
static void print_entry(uint32_t entry) {
    const uint8_t byte = I2C_LOG_BYTE(entry);
    const uint8_t value = I2C_LOG_VALUE(entry);
    const char *dir = (byte & 1u) ? "read" : "write";

    switch ((i2c_log_event_t)I2C_LOG_EVENT(entry)) {
    case I2C_LOG_ADDR_READ:
        printf("  0x%02X %-5s -> ACK\n", byte >> 1, dir);
        break;
    case I2C_LOG_TX_SENT:
        printf("                sent 0x%02X (register 0x%02X)\n", byte, value);
        break;
    case I2C_LOG_TX_MISMATCH:
        printf("                sent 0x%02X, but the bus carried 0x%02X\n", value, byte);
        break;
    case I2C_LOG_ADDR_WRITE:
        printf("  0x%02X %-5s -> ACK\n", byte >> 1, dir);
        break;
    case I2C_LOG_ADDR_UNKNOWN:
        printf("  0x%02X %-5s -> NACK (no device at this address)\n", byte >> 1, dir);
        break;
    case I2C_LOG_DATA_POINTER:
        printf("                pointer set to 0x%02X\n", byte);
        break;
    case I2C_LOG_DATA_REFUSED:
        printf("                then 0x%02X -> NACK (the map is read-only)\n", byte);
        break;
    case I2C_LOG_DATA_STRAY:
        printf("  data 0x%02X outside any transaction -> NACK\n", byte);
        break;
    }
}

// Print everything the IRQ handler has logged since the last call.
static void drain_log(void) {
    const uint32_t head = g_bus.log_head;
    __dmb(); // the entries up to head are complete before we read them
    uint32_t tail = g_bus.log_tail;
    while (tail != head) {
        print_entry(g_bus.log[tail & (I2C_BUS_LOG_SIZE - 1u)]);
        tail++;
    }
    __dmb(); // finished reading before the handler may reuse these slots
    g_bus.log_tail = tail;
}
#endif

// Failures, counted in every build. None of them can happen while the
// handshake behaves; each means the bus was held low, or still is.
typedef struct {
    uint32_t stray, no_addr, blocked, mismatch;
} problems_t;

static void report_problems(problems_t *seen) {
    const problems_t now = {g_bus.nacked_stray, g_bus.no_address, g_bus.tx_blocked,
                            g_bus.tx_mismatch};
    if (now.stray != seen->stray) {
        printf("  !! PROBLEM: %lu data bytes arrived outside any transaction\n",
               (unsigned long)(now.stray - seen->stray));
    }
    if (now.no_addr != seen->no_addr) {
        printf("  !! PROBLEM: %lu handshakes with no byte to read\n",
               (unsigned long)(now.no_addr - seen->no_addr));
    }
    if (now.blocked != seen->blocked) {
        printf("  !! PROBLEM: %lu handshakes with no room to answer: the bus may be stuck\n",
               (unsigned long)(now.blocked - seen->blocked));
    }
    if (now.mismatch != seen->mismatch) {
        printf("  !! PROBLEM: %lu bytes sent read back different: something else drove SDA\n",
               (unsigned long)(now.mismatch - seen->mismatch));
    }
    *seen = now;
}

int main(void) {
    stdio_init_all();

    emuwire_status_t st = i2c_bus_init(&g_bus, pio0, 0, PIN_SDA, BUS_HZ);
    if (st == EMUWIRE_STATUS_OK) {
        // Like a real BMP280: the pointer moves on after each byte read.
        st = i2c_bus_attach(&g_bus, OUR_ADDR, OUR_DEVICE_ID, bmp280_regs, sizeof bmp280_regs,
                            EMUWIRE_AUTO_INCREMENT_ON_READ, EMUWIRE_DEVICE_FLAGS_WRAP);
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

    problems_t problems = {0, 0, 0, 0};
    bool was_connected = true;
#if I2C_BUS_VERBOSE
    uint32_t dropped_seen = 0;
#endif

    while (true) {
#if I2C_BUS_VERBOSE
        drain_log();

        // A full log loses entries; say how many rather than skip them.
        const uint32_t dropped = g_bus.log_dropped;
        if (dropped != dropped_seen) {
            printf("  ... %lu calls not shown: the log was full\n",
                   (unsigned long)(dropped - dropped_seen));
            dropped_seen = dropped;
        }
#endif
        report_problems(&problems);

        // The device's mode is read by core 1 on every byte; a single-byte
        // store here is seen there whole.
        const int key = getchar_timeout_us(0);
        if (key == 'a' || key == 'w') {
            i2c_device_t *dev = &g_bus.devices[0];
            if (key == 'a') {
                dev->auto_increment = (uint8_t)((dev->auto_increment + 1u) % 4u);
            } else {
                dev->flags ^= EMUWIRE_DEVICE_FLAGS_WRAP;
            }
            printf("\n");
            print_pointer_mode();
            printf("\n");
        }

        // Reopening the serial port otherwise shows a blank screen, with no
        // way to tell a running slave from a dead one.
        const bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            print_banner();
        }
        was_connected = connected;

        sleep_ms(10);
    }
}
