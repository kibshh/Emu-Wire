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
    printf("  core 1    serving the bus\n");
#if I2C_BUS_VERBOSE
    printf("  log       every call on the bus, as it happens (verbose build)\n\n");
#else
    printf("  log       failures only (quiet build: I2C_BUS_VERBOSE=OFF)\n\n");
#endif
}

#if I2C_BUS_VERBOSE
// One line per address call. The data bytes that follow it are added to the
// same line, so a register access reads as one line.
static bool g_line_open;

static void end_line(void) {
    if (g_line_open) {
        printf("\n");
        g_line_open = false;
    }
}

static void print_entry(uint32_t entry) {
    const uint8_t byte = I2C_LOG_BYTE(entry);
    const uint8_t value = I2C_LOG_VALUE(entry);
    const char *dir = (byte & 1u) ? "read" : "write";

    switch ((i2c_log_event_t)I2C_LOG_EVENT(entry)) {
    case I2C_LOG_ADDR_READ:
        end_line();
        printf("  0x%02X %-5s -> ACK, sent 0x%02X", byte >> 1, dir, value);
        g_line_open = true;
        break;
    case I2C_LOG_ADDR_WRITE:
        end_line();
        printf("  0x%02X %-5s -> ACK", byte >> 1, dir);
        g_line_open = true;
        break;
    case I2C_LOG_ADDR_UNKNOWN:
        end_line();
        printf("  0x%02X %-5s -> NACK (no device at this address)", byte >> 1, dir);
        g_line_open = true;
        break;
    case I2C_LOG_DATA_POINTER:
        printf(", pointer set to 0x%02X", byte);
        break;
    case I2C_LOG_DATA_REFUSED:
        printf(", then 0x%02X -> NACK (the map is read-only)", byte);
        break;
    case I2C_LOG_DATA_STRAY:
        end_line();
        printf("  data 0x%02X outside any transaction -> NACK", byte);
        g_line_open = true;
        break;
    }
}

// Print everything the IRQ handler has logged since the last call. Returns
// whether there was anything.
static bool drain_log(void) {
    const uint32_t head = g_bus.log_head;
    __dmb(); // the entries up to head are complete before we read them
    uint32_t tail = g_bus.log_tail;
    if (tail == head) {
        return false;
    }
    while (tail != head) {
        print_entry(g_bus.log[tail & (I2C_BUS_LOG_SIZE - 1u)]);
        tail++;
    }
    __dmb(); // finished reading before the handler may reuse these slots
    g_bus.log_tail = tail;
    return true;
}
#endif

// Failures, counted in every build. None of them can happen while the
// handshake behaves; each means the bus was held low, or still is.
typedef struct {
    uint32_t stray, no_addr, blocked;
} problems_t;

static void report_problems(problems_t *seen) {
    const problems_t now = {g_bus.nacked_stray, g_bus.no_address, g_bus.tx_blocked};
    if (now.stray != seen->stray) {
        printf("\n  !! PROBLEM: %lu data bytes arrived outside any transaction\n",
               (unsigned long)(now.stray - seen->stray));
    }
    if (now.no_addr != seen->no_addr) {
        printf("\n  !! PROBLEM: %lu handshakes with no byte to read\n",
               (unsigned long)(now.no_addr - seen->no_addr));
    }
    if (now.blocked != seen->blocked) {
        printf("\n  !! PROBLEM: %lu handshakes with no room to answer: the bus may be stuck\n",
               (unsigned long)(now.blocked - seen->blocked));
    }
    *seen = now;
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

    problems_t problems = {0, 0, 0};
    bool was_connected = true;
#if I2C_BUS_VERBOSE
    uint32_t dropped_seen = 0;
    uint32_t last_entry_ms = 0;
#endif

    while (true) {
#if I2C_BUS_VERBOSE
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (drain_log()) {
            last_entry_ms = now_ms;
        } else if (g_line_open && now_ms - last_entry_ms >= 100) {
            // Nothing more came for this call: finish its line.
            end_line();
        }

        // A full log loses entries; say how many rather than skip them.
        const uint32_t dropped = g_bus.log_dropped;
        if (dropped != dropped_seen) {
            end_line();
            printf("  ... %lu calls not shown: the log was full\n",
                   (unsigned long)(dropped - dropped_seen));
            dropped_seen = dropped;
        }
#endif
        report_problems(&problems);

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
