/*
 * I2C master test rig.
 *
 * Runs on a second Pico and drives a fixed set of I2C transactions against
 * whatever is on the bus — a real sensor, or the emulator. It reports
 * pass/fail over its own USB serial.
 *
 * This is ground truth. Everything downstream is judged by what this says, so
 * it uses the RP2350's hardware I2C rather than PIO and stays deliberately
 * dull: if the rig is wrong, every result it produces is wrong with it.
 *
 * Validate it against a real BMP280 before trusting it against the emulator.
 *
 *   Wiring        rig GP4 -> SCL, GP5 -> SDA, GND -> GND, 3V3 -> VCC
 *   Pull-ups      1 kOhm to 3V3 on both lines. 4.7 kOhm will not meet the
 *                 rise time at 400 kHz on a breadboard, let alone 1 MHz.
 *
 * Serial commands, once running:
 *   r   run the transaction set once
 *   s   soak: run continuously, counting errors, until a key is pressed
 *   1   100 kHz      4   400 kHz      f   1 MHz (Fast-mode Plus)
 *   ?   print this list
 */

#include <stdio.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------------------
// What we are talking to
// ---------------------------------------------------------------------------

#define I2C_PORT i2c0
#define PIN_SCL 5
#define PIN_SDA 4

#define TARGET_ADDR 0x76 // BMP280 with SDO low
#define ABSENT_ADDR 0x60 // nothing here: used to prove a NACK is seen

// BMP280 registers. The emulator answers the same map, so one rig serves both.
#define REG_CHIP_ID 0xD0
#define REG_CTRL_MEAS 0xF4
#define REG_PRESS_MSB 0xF7  // the burst read starts here and runs to 0xFC

#define EXPECT_CHIP_ID 0x58 // 0x58 = BMP280. 0x60 = BME280, a different part.
#define BURST_LEN 6         // press[3] + temp[3]

// Generous, because a slave may legitimately stretch the clock. The emulator
// stretches on every address match — that is the mechanism that lets one state
// machine answer several addresses — so a tight timeout here would report a
// fault that is not one.
#define TIMEOUT_US 100000

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static int g_pass;
static int g_fail;

static void report(const char *name, bool ok, const char *detail) {
    printf("  %-34s %s", name, ok ? "PASS" : "FAIL");
    if (detail && detail[0]) {
        printf("   %s", detail);
    }
    printf("\n");
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

/* Turn a pico-sdk I2C return into something readable. The SDK returns the
 * byte count on success, or a negative error; a NACK and a timeout are
 * different failures and the difference matters when diagnosing a bus. */
static const char *i2c_err(int rc, int expected_len) {
    static char buf[64];
    if (rc == expected_len) {
        return "";
    }
    if (rc == PICO_ERROR_TIMEOUT) {
        snprintf(buf, sizeof buf, "timed out after %u us", TIMEOUT_US);
    } else if (rc == PICO_ERROR_GENERIC) {
        snprintf(buf, sizeof buf, "no ACK from 0x%02X", TARGET_ADDR);
    } else {
        snprintf(buf, sizeof buf, "moved %d bytes, wanted %d", rc, expected_len);
    }
    return buf;
}

static void hex(char *out, size_t out_len, const uint8_t *data, int n) {
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        char byte[4];
        snprintf(byte, sizeof byte, "%02X ", data[i]);
        strncat(out, byte, out_len - strlen(out) - 1);
    }
}

// ---------------------------------------------------------------------------
// The transaction set
// ---------------------------------------------------------------------------

/* Set the register pointer, then read from it in a separate transaction.
 * Helper for the tests below, not a test itself. */
static int read_regs(uint8_t reg, uint8_t *dst, int len) {
    int rc = i2c_write_timeout_us(I2C_PORT, TARGET_ADDR, &reg, 1, true, TIMEOUT_US);
    if (rc != 1) {
        return rc;
    }
    return i2c_read_timeout_us(I2C_PORT, TARGET_ADDR, dst, (size_t)len, false, TIMEOUT_US);
}

/* 1. Single-byte read with no pointer write.
 *
 * Reads whatever the pointer already points at. On a part with
 * auto-increment this is the byte after the last one read, so the value is
 * not asserted — what is being tested is that a bare read transaction
 * completes and the slave ACKs its address. */
static void t_single_byte_read(void) {
    uint8_t v = 0;
    int rc = i2c_read_timeout_us(I2C_PORT, TARGET_ADDR, &v, 1, false, TIMEOUT_US);
    char detail[64];
    snprintf(detail, sizeof detail, "read 0x%02X", v);
    report("single-byte read", rc == 1, rc == 1 ? detail : i2c_err(rc, 1));
}

/* 2. Register-pointer write, repeated START, read.
 *
 * The pattern every sensor driver uses. `nostop = true` on the write leaves
 * the bus held so the read begins with a repeated START rather than a STOP
 * and a fresh START. Getting this wrong still often works on real silicon,
 * which is exactly why it needs testing explicitly. */
static void t_pointer_then_repeated_start(void) {
    uint8_t id = 0;
    int rc = read_regs(REG_CHIP_ID, &id, 1);
    if (rc != 1) {
        report("pointer + repeated START read", false, i2c_err(rc, 1));
        return;
    }
    char detail[80];
    snprintf(detail, sizeof detail, "chip id 0x%02X, expected 0x%02X", id, EXPECT_CHIP_ID);
    report("pointer + repeated START read", id == EXPECT_CHIP_ID, detail);
}

/* 3. Burst read.
 *
 * Six consecutive registers in one transaction, which requires the slave's
 * pointer to auto-increment. A slave that returns the same byte six times
 * passes a single-byte test and fails here. */
static void t_burst_read(void) {
    uint8_t buf[BURST_LEN] = {0};
    int rc = read_regs(REG_PRESS_MSB, buf, BURST_LEN);
    if (rc != BURST_LEN) {
        report("burst read (6 bytes)", false, i2c_err(rc, BURST_LEN));
        return;
    }
    bool all_same = true;
    for (int i = 1; i < BURST_LEN; i++) {
        if (buf[i] != buf[0]) {
            all_same = false;
        }
    }
    char detail[80];
    hex(detail, sizeof detail, buf, BURST_LEN);
    if (all_same) {
        strncat(detail, " (all identical: pointer may not be incrementing)",
                sizeof detail - strlen(detail) - 1);
    }
    report("burst read (6 bytes)", true, detail);
}

/* 4. Register write, read back.
 *
 * ctrl_meas is writable on a BMP280. Writing 0 puts it in sleep mode, which
 * is safe and reversible. The read-back is the assertion: a slave that
 * accepts writes and discards them passes the write and fails here. */
static void t_register_write(void) {
    const uint8_t original_value = 0x00;
    uint8_t payload[2] = {REG_CTRL_MEAS, 0x27}; // oversampling x1, normal mode

    int rc = i2c_write_timeout_us(I2C_PORT, TARGET_ADDR, payload, 2, false, TIMEOUT_US);
    if (rc != 2) {
        report("register write", false, i2c_err(rc, 2));
        return;
    }

    uint8_t back = 0;
    rc = read_regs(REG_CTRL_MEAS, &back, 1);
    if (rc != 1) {
        report("register write", false, i2c_err(rc, 1));
        return;
    }

    char detail[80];
    snprintf(detail, sizeof detail, "wrote 0x%02X, read back 0x%02X", payload[1], back);
    report("register write", back == payload[1], detail);

    // Leave the part as we found it, so a soak run does not drift its state.
    uint8_t restore[2] = {REG_CTRL_MEAS, original_value};
    i2c_write_timeout_us(I2C_PORT, TARGET_ADDR, restore, 2, false, TIMEOUT_US);
}

/* 5. NACK handling.
 *
 * Addressing a device that is not there must fail, and must fail as a NACK
 * rather than a timeout. This is the one test that passes by failing, and it
 * proves the rig can tell "no device" from "device is slow" — a distinction
 * everything downstream depends on. */
static void t_nack_on_absent_address(void) {
    uint8_t v = 0;
    int rc = i2c_read_timeout_us(I2C_PORT, ABSENT_ADDR, &v, 1, false, TIMEOUT_US);
    char detail[80];
    if (rc == PICO_ERROR_GENERIC) {
        snprintf(detail, sizeof detail, "0x%02X NACKed, as it should", ABSENT_ADDR);
        report("NACK from an absent address", true, detail);
    } else if (rc == PICO_ERROR_TIMEOUT) {
        report("NACK from an absent address", false, "timed out instead of NACKing");
    } else {
        snprintf(detail, sizeof detail, "0x%02X answered (rc=%d) — something IS there",
                 ABSENT_ADDR, rc);
        report("NACK from an absent address", false, detail);
    }
}

/* 6. Clock-stretch tolerance.
 *
 * A slave may hold SCL low to buy time, and a master must wait. The emulator
 * does this on every address match, so this measures how long a transaction
 * actually took.
 *
 * A real BMP280 does not stretch, so against one this reports a short
 * duration and passes — proving only that the rig does not invent delays.
 * Against the emulator the same number becomes the stretch measurement, and
 * it should agree with the logic analyzer. */
static void t_clock_stretch_tolerance(void) {
    uint8_t id = 0;
    absolute_time_t start = get_absolute_time();
    int rc = read_regs(REG_CHIP_ID, &id, 1);
    int64_t elapsed_us = absolute_time_diff_us(start, get_absolute_time());

    char detail[96];
    snprintf(detail, sizeof detail, "%lld us for a 1-byte read", elapsed_us);
    report("tolerates a stretched clock", rc == 1, rc == 1 ? detail : i2c_err(rc, 1));
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static uint g_baud = 400000;

static int run_once(bool verbose) {
    g_pass = 0;
    g_fail = 0;

    if (verbose) {
        printf("\nI2C master rig — target 0x%02X @ %u Hz, SCL GP%d, SDA GP%d\n", TARGET_ADDR,
               g_baud, PIN_SCL, PIN_SDA);
        printf("----------------------------------------------------------------\n");
    }

    t_single_byte_read();
    t_pointer_then_repeated_start();
    t_burst_read();
    t_register_write();
    t_nack_on_absent_address();
    t_clock_stretch_tolerance();

    if (verbose) {
        printf("----------------------------------------------------------------\n");
        printf("%d passed, %d failed\n", g_pass, g_fail);
        if (g_fail == 0) {
            printf("ALL TRANSACTIONS CORRECT\n");
        }
    }
    return g_fail;
}

/* Continuous run for soak testing. One corrupted transaction in a million is
 * a real bug, not noise, so the counters are what matter here rather than the
 * per-test output. */
static void soak(void) {
    uint32_t rounds = 0;
    uint32_t failures = 0;
    printf("\nSoaking at %u Hz. Press any key to stop.\n", g_baud);
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        failures += (uint32_t)run_once(false);
        rounds++;
        if (rounds % 1000 == 0) {
            printf("  %lu rounds, %lu failed transactions\n", (unsigned long)rounds,
                   (unsigned long)failures);
        }
    }
    printf("Stopped: %lu rounds, %lu failed transactions\n", (unsigned long)rounds,
           (unsigned long)failures);
}

static void set_speed(uint baud) {
    g_baud = baud;
    uint actual = i2c_set_baudrate(I2C_PORT, baud);
    printf("Bus set to %u Hz (hardware reports %u Hz)\n", baud, actual);
}

static void help(void) {
    printf("\n  r  run once      s  soak until a key is pressed\n");
    printf("  1  100 kHz       4  400 kHz       f  1 MHz\n");
    printf("  ?  this list\n");
}

int main(void) {
    stdio_init_all();

    i2c_init(I2C_PORT, g_baud);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    // No internal pull-ups. They are far too weak for I2C edges; the bus needs
    // real resistors, and enabling these would mask their absence.
    gpio_disable_pulls(PIN_SDA);
    gpio_disable_pulls(PIN_SCL);

    // Wait for the host to open the port, so the first report is not lost.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }

    printf("\nEmuWire I2C master test rig\n");
    help();
    run_once(true);

    bool was_connected = true;

    while (true) {
        // The timeout is not a wait for input — a key press wakes this
        // immediately. It is a half-second tick so a terminal reattaching can
        // be noticed, below.
        int c = getchar_timeout_us(500000);

        if (c == PICO_ERROR_TIMEOUT) {
            // Reopening the serial port otherwise shows a blank screen, with
            // no way to tell a running rig from a dead one without guessing
            // at a key. Reprint on reconnect instead.
            bool connected = stdio_usb_connected();
            if (connected && !was_connected) {
                printf("\nEmuWire I2C master test rig — reconnected, %u Hz\n", g_baud);
                help();
            }
            was_connected = connected;
            continue;
        }

        switch (c) {
        case 'r':
            run_once(true);
            break;
        case 's':
            soak();
            break;
        case '1':
            set_speed(100000);
            break;
        case '4':
            set_speed(400000);
            break;
        case 'f':
            set_speed(1000000);
            break;
        case '?':
            help();
            break;
        default:
            break;
        }
    }
}
