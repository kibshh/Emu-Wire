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

// A second device with no registers at all: every read returns its sample,
// from the first byte. Made up, not a real part; just two distinct bytes.
#define STREAM_ADDR 0x40
#define STREAM_DEVICE_ID 2
static uint8_t stream_sample[2] = {0x12, 0x34};

/*
 * Enough of a BMP280 to be recognised. The chip id is the real one; the
 * measurement registers hold fixed, made-up values, so a burst read has
 * something to return that is not all the same byte. The real device model,
 * with writable registers and manifests behind it, comes later.
 */
#define REG_CHIP_ID 0xD0
static uint8_t bmp280_regs[256] = {
    [REG_CHIP_ID] = 0x58,                          // what a BMP280 answers
    [0xF7] = 0x51, [0xF8] = 0x2C, [0xF9] = 0x80,   // pressure  msb, lsb, xlsb
    [0xFA] = 0x82, [0xFB] = 0x3A, [0xFC] = 0x00,   // temperature
    // Markers either side of the end of the map, not BMP280 values: a burst
    // from 0xFE shows whether the pointer wraps (E0 E1 E2 E3) or saturates
    // on the last register (E0 E1 E1 E1).
    [0xFE] = 0xE0, [0xFF] = 0xE1, [0x00] = 0xE2, [0x01] = 0xE3,
};

// The registers a BMP280 lets the master write: reset, ctrl_meas, config.
// Everything else is read-only.
#define W EMUWIRE_REGISTER_FLAGS_WRITABLE
static const uint8_t bmp280_flags[256] = {
    [0xE0] = W, [0xF4] = W, [0xF5] = W,
};
#undef W

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

// What the rig's tests 7 and 9 should read under the current settings.
static void print_pointer_mode(void) {
    const i2c_device_t *dev = &g_bus.devices[0];
    const uint8_t mode = dev->auto_increment;
    const bool wrap = (dev->flags & EMUWIRE_DEVICE_FLAGS_WRAP) != 0;
    const bool on_read = mode == EMUWIRE_AUTO_INCREMENT_ON_READ || mode == EMUWIRE_AUTO_INCREMENT_BOTH;
    const bool on_write = mode == EMUWIRE_AUTO_INCREMENT_ON_WRITE || mode == EMUWIRE_AUTO_INCREMENT_BOTH;

    // Test 9 writes 27 10 from 0xF4, then reads 2 bytes back from 0xF4.
    // In pairs, 10 is the next register number, so only F4 = 27 changes.
    // Otherwise writes that move put 27 in F4 and 10 in F5, and writes that
    // stay leave 10 in F4. Reads that move then return F4 F5; reads that
    // stay return F4 F4.
    const bool pairs = (dev->flags & EMUWIRE_DEVICE_FLAGS_WRITE_PAIRS) != 0;
    const char *burst_write = pairs      ? (on_read ? "27 00" : "27 27")
                              : on_write ? (on_read ? "27 10" : "27 27")
                                         : (on_read ? "10 00" : "10 10");

    printf("  pointer   auto-increment %s, %s at the end of the map\n", mode_name(mode),
           wrap ? "wraps to 0x00" : "stays on 0xFF");
    printf("            rig test 7 (burst from 0xFE) should read %s\n",
           !on_read ? "E0 E0 E0 E0" : wrap ? "E0 E1 E2 E3" : "E0 E1 E1 E1");
    printf("            rig test 9 (burst write 27 10) should read back %s\n", burst_write);
    printf("  writes    0xE0, 0xF4, 0xF5 writable; to any other register %s\n",
           (dev->flags & EMUWIRE_DEVICE_FLAGS_NACK_ON_RO_WRITE) ? "NACKed" : "ACKed and ignored");
    printf("            %s\n", pairs ? "in register/value pairs, like a real BMP280"
                                     : "a start register, then values");
}

static void print_banner(void) {
    printf("\nEmuWire PIO I2C slave\n");
    printf("  program   %d of 32 PIO instructions, loaded at offset %u\n",
           i2c_slave_program.length, g_bus.offset);
    printf("  pins      SDA = GP%d, SCL = GP%d\n", PIN_SDA, PIN_SDA + 1);
    printf("  device    0x%02X, no registers: every read returns %02X %02X\n", STREAM_ADDR,
           stream_sample[0], stream_sample[1]);
    printf("  device    0x%02X, a BMP280 register map (0x%02X reads 0x%02X)\n", OUR_ADDR,
           REG_CHIP_ID, bmp280_regs[REG_CHIP_ID]);
    print_pointer_mode();
    printf("  keys      a = next auto-increment mode, w = toggle wrap,\n");
    printf("            n = toggle NACK on read-only writes, p = toggle pair writes\n");
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
    case I2C_LOG_DATA_WRITTEN:
        printf("                wrote 0x%02X to register 0x%02X\n", byte, value);
        break;
    case I2C_LOG_DATA_IGNORED:
        printf("                0x%02X to register 0x%02X ignored (read-only)\n", byte, value);
        break;
    case I2C_LOG_DATA_NO_REGISTERS:
        printf("                0x%02X ignored (this device has no registers)\n", byte);
        break;
    case I2C_LOG_DATA_REFUSED:
        printf("                0x%02X to register 0x%02X -> NACK (read-only)\n", byte, value);
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
        // As measured on a real BMP280 (rig tests 8 and 9, 2026-10-10): a
        // write to a read-only register is ACKed and ignored, and a write is
        // register/value pairs.
        const i2c_device_config_t bmp280 = {
            .addr = OUR_ADDR,
            .device_id = OUR_DEVICE_ID,
            .regs = bmp280_regs,
            .reg_flags = bmp280_flags,
            .reg_count = sizeof bmp280_regs,
            .reg_addr_width = EMUWIRE_REG_ADDR_WIDTH_WIDTH_8,
            .auto_increment = EMUWIRE_AUTO_INCREMENT_ON_READ,
            .flags = EMUWIRE_DEVICE_FLAGS_WRAP | EMUWIRE_DEVICE_FLAGS_WRITE_PAIRS,
        };
        st = i2c_bus_attach(&g_bus, &bmp280);
    }
    if (st == EMUWIRE_STATUS_OK) {
        // Past the end of its sample it stays on the last byte.
        const i2c_device_config_t stream = {
            .addr = STREAM_ADDR,
            .device_id = STREAM_DEVICE_ID,
            .regs = stream_sample,
            .reg_flags = NULL,
            .reg_count = sizeof stream_sample,
            .reg_addr_width = EMUWIRE_REG_ADDR_WIDTH_STREAMING,
            .auto_increment = EMUWIRE_AUTO_INCREMENT_NONE,
            .flags = 0,
        };
        st = i2c_bus_attach(&g_bus, &stream);
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
        if (key == 'a' || key == 'w' || key == 'n' || key == 'p') {
            i2c_device_t *dev = &g_bus.devices[0];
            if (key == 'a') {
                dev->auto_increment = (uint8_t)((dev->auto_increment + 1u) % 4u);
            } else if (key == 'w') {
                dev->flags ^= EMUWIRE_DEVICE_FLAGS_WRAP;
            } else if (key == 'n') {
                dev->flags ^= EMUWIRE_DEVICE_FLAGS_NACK_ON_RO_WRITE;
            } else {
                dev->flags ^= EMUWIRE_DEVICE_FLAGS_WRITE_PAIRS;
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
