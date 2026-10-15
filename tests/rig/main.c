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
 *   s   soak: run the set over and over until a million transactions,
 *       printing failures only; any key stops it early
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
#define REG_CONFIG 0xF5     // writable too: standby time and filter
#define REG_PRESS_MSB 0xF7  // the burst read starts here and runs to 0xFC

#define EXPECT_CHIP_ID 0x58 // 0x58 = BMP280. 0x60 = BME280, a different part.
#define BURST_LEN 6         // press[3] + temp[3]
#define REG_END_OF_MAP 0xFE // 2 registers before the 8-bit pointer runs out

// The bring-up slave's second device: no registers, and every read returns
// this sample from its first byte. Not on the bus with a real BMP280.
#define STREAM_ADDR 0x40
static const uint8_t STREAM_SAMPLE[2] = {0x12, 0x34};

// The rest of the bring-up slave's bus, for the multi-device tests: a second
// BMP280 (SDO high) and an MPU-6050. Not there with a lone BMP280 either.
#define SECOND_ADDR 0x77
#define MPU_ADDR 0x68
#define MPU_WHO_AM_I 0x75
#define EXPECT_MPU_ID 0x68

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
static int g_skip;

// The address of the last call, so an error message names the device that
// failed rather than always the BMP280.
static uint8_t g_addr = TARGET_ADDR;

// Every I2C transaction the rig has started: each SDK call is one, from its
// START to its STOP or repeated START. A soak counts these.
static uint32_t g_transactions;

// A soak prints failures only, and only the first few: a loose wire can fail
// every round, and a screen of identical lines hides the first one.
#define SOAK_FAILS_SHOWN 20u
static bool g_quiet;
static uint32_t g_fails_shown;

static void report(const char *name, bool ok, const char *detail) {
    if (g_quiet) {
        if (ok) {
            g_pass++;
            return;
        }
        g_fail++;
        if (g_fails_shown < SOAK_FAILS_SHOWN) {
            g_fails_shown++;
            printf("  FAIL near transaction %lu: %s   %s\n", (unsigned long)g_transactions, name,
                   detail ? detail : "");
        }
        return;
    }
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

/* A test that needs a device the bus does not have. Neither a pass nor a
 * failure: running the rig against a real BMP280 is not wrong, it just cannot
 * test what only the emulator has. */
static void report_skip(const char *name, const char *detail) {
    if (!g_quiet) {
        printf("  %-34s SKIP   %s\n", name, detail);
    }
    g_skip++;
}

/* Turn a pico-sdk I2C return into something readable. The SDK returns the
 * byte count on success, or a negative error; a NACK and a timeout are
 * different failures and the difference matters when diagnosing a bus. */
// Returned by rig_write() when the SDK reported success but the controller
// had latched a NACK on a data byte. See rig_write().
#define RIG_ERR_LATE_NACK (-1000)

static const char *i2c_err(int rc, int expected_len) {
    static char buf[64];
    if (rc == expected_len) {
        return "";
    }
    if (rc == PICO_ERROR_TIMEOUT) {
        snprintf(buf, sizeof buf, "timed out after %u ms (a line held low?)", TIMEOUT_US / 1000);
    } else if (rc == PICO_ERROR_GENERIC) {
        snprintf(buf, sizeof buf, "no ACK from 0x%02X (nothing answered)", g_addr);
    } else if (rc == RIG_ERR_LATE_NACK) {
        snprintf(buf, sizeof buf, "a data byte was NACKed (the SDK missed it)");
    } else {
        // The SDK reports a NACKed data byte as the count that got through.
        snprintf(buf, sizeof buf, "NACKed after %d of %d bytes", rc, expected_len);
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

/* i2c_write_timeout_us(), plus the NACK it can miss.
 *
 * The SDK checks for a NACK as soon as a byte has been shifted out, before
 * the ninth clock. A target that stretches the clock before answering — the
 * emulator does, on every byte — NACKs after that check, so the SDK reports
 * success. The abort then stays latched in the controller and makes the next
 * call fail instead, with a timeout. Found on the bench.
 *
 * So after every write, look at the controller ourselves: a latched abort
 * means the write did not all get through. Clear it, so it cannot poison the
 * next call, and say what really happened.
 *
 * It can only catch a NACK that has happened by the time the SDK returns. For
 * a write ending in STOP the SDK waits for the STOP, which comes after the
 * NACK, so it always has. For a write with nostop it may not have — but the
 * rig only uses those for register pointers, which a target does not refuse. */
static int rig_write_at(uint8_t addr, const uint8_t *src, size_t len, bool nostop) {
    i2c_hw_t *hw = i2c_get_hw(I2C_PORT);
    g_addr = addr;
    g_transactions++;
    int rc = i2c_write_timeout_us(I2C_PORT, addr, src, len, nostop, TIMEOUT_US);
    if (hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
        const uint32_t reason = hw->tx_abrt_source;
        (void)hw->clr_tx_abrt; // reading it clears the abort
        if (rc == (int)len) {
            rc = (reason & I2C_IC_TX_ABRT_SOURCE_ABRT_7B_ADDR_NOACK_BITS) ? PICO_ERROR_GENERIC
                                                                          : RIG_ERR_LATE_NACK;
        }
    }
    return rc;
}

static int rig_write(const uint8_t *src, size_t len, bool nostop) {
    return rig_write_at(TARGET_ADDR, src, len, nostop);
}

/* A read ending in STOP, from wherever the target's pointer already is. */
static int rig_read_at(uint8_t addr, uint8_t *dst, int len) {
    g_addr = addr;
    g_transactions++;
    return i2c_read_timeout_us(I2C_PORT, addr, dst, (size_t)len, false, TIMEOUT_US);
}

/* Set the register pointer, then read from it in a separate transaction.
 * Helper for the tests below, not a test itself. */
static int read_regs_at(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    int rc = rig_write_at(addr, &reg, 1, true);
    if (rc != 1) {
        return rc;
    }
    return rig_read_at(addr, dst, len);
}

static int read_regs(uint8_t reg, uint8_t *dst, int len) {
    return read_regs_at(TARGET_ADDR, reg, dst, len);
}

/* 1. Single-byte read with no pointer write.
 *
 * Reads whatever the pointer already points at. On a part with
 * auto-increment this is the byte after the last one read, so the value is
 * not asserted — what is being tested is that a bare read transaction
 * completes and the slave ACKs its address. */
static void t_single_byte_read(void) {
    uint8_t v = 0;
    int rc = rig_read_at(TARGET_ADDR, &v, 1);
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
    bool tail_ff = true;
    for (int i = 1; i < BURST_LEN; i++) {
        if (buf[i] != 0xFF) {
            tail_ff = false;
        }
    }
    if (all_same) {
        strncat(detail, " (all identical: pointer may not be incrementing)",
                sizeof detail - strlen(detail) - 1);
    } else if (tail_ff) {
        // 0xFF is what a released bus reads, so this is one real byte and
        // then nobody sending.
        strncat(detail, " (only the first byte is real: the rest is an idle bus)",
                sizeof detail - strlen(detail) - 1);
    }
    // Either one means the target did not send six registers: a real BMP280
    // does neither.
    report("burst read (6 bytes)", !all_same && !tail_ff, detail);
}

/* 8. Write to a read-only register.
 *
 * The chip id cannot be changed on a BMP280. Write to it, then read it back:
 * it must still be the chip id. Whether the write itself was ACKed and ignored
 * or NACKed differs between parts, so both pass; the detail says which, and
 * against a real BMP280 that is how to find out what the real part does. */
static void t_read_only_write(void) {
    const uint8_t payload[2] = {REG_CHIP_ID, 0x00};
    const int wrc = rig_write(payload, 2, false);
    const bool acked = wrc == 2;
    if (!acked && wrc != 1 && wrc != RIG_ERR_LATE_NACK) {
        report("write to a read-only register", false, i2c_err(wrc, 2));
        return;
    }

    uint8_t back = 0;
    const int rc = read_regs(REG_CHIP_ID, &back, 1);
    if (rc != 1) {
        report("write to a read-only register", false, i2c_err(rc, 1));
        return;
    }

    char detail[80];
    snprintf(detail, sizeof detail, "write %s, chip id still 0x%02X",
             acked ? "ACKed and ignored" : "NACKed", back);
    report("write to a read-only register", back == EXPECT_CHIP_ID, detail);
}

/* 9. Burst write: two values in one transaction.
 *
 * Writes 0x27 then 0x10 starting at ctrl_meas, and reads two bytes back from
 * there. Where the second value lands depends on the target: a pointer that
 * moves on write puts it in the next register, one that does not overwrites
 * the first. A real BMP280 takes writes as register/value pairs, so to it the
 * second byte is a register number with no value, and nothing changes. All
 * are legitimate, so this prints what came back, to compare with how the
 * target is configured. Both registers are restored afterwards. */
static void t_burst_write(void) {
    const uint8_t payload[3] = {REG_CTRL_MEAS, 0x27, 0x10};
    const int wrc = rig_write(payload, 3, false);
    if (wrc != 3) {
        report("burst write (2 bytes)", false, i2c_err(wrc, 3));
        return;
    }

    uint8_t back[2] = {0};
    const int rc = read_regs(REG_CTRL_MEAS, back, 2);

    const uint8_t restore_meas[2] = {REG_CTRL_MEAS, 0x00};
    const uint8_t restore_config[2] = {REG_CONFIG, 0x00};
    rig_write(restore_meas, 2, false);
    rig_write(restore_config, 2, false);

    if (rc != 2) {
        report("burst write (2 bytes)", false, i2c_err(rc, 2));
        return;
    }
    char detail[80];
    hex(detail, sizeof detail, back, 2);
    strncat(detail, " (wrote 27 10 from 0xF4: where did 10 land?)",
            sizeof detail - strlen(detail) - 1);
    report("burst write (2 bytes)", true, detail);
}

/* 10. Raw read from a device with no registers.
 *
 * Some devices have no register pointer: a read just returns the current
 * sample, from its first byte. Read it twice with no pointer write in
 * between; both must start at the beginning, or the device is behaving as if
 * it had a pointer after all. */
static void t_streaming_read(void) {
    uint8_t first[2] = {0};
    uint8_t second[2] = {0};
    int rc = rig_read_at(STREAM_ADDR, first, 2);
    if (rc == PICO_ERROR_GENERIC) {
        report_skip("raw read, no registers (0x40)", "nothing at 0x40");
        return;
    }
    if (rc == 2) {
        rc = rig_read_at(STREAM_ADDR, second, 2);
    }
    if (rc != 2) {
        report("raw read, no registers (0x40)", false, i2c_err(rc, 2));
        return;
    }

    char detail[80];
    char half[16];
    hex(half, sizeof half, first, 2);
    snprintf(detail, sizeof detail, "%sthen ", half);
    hex(half, sizeof half, second, 2);
    strncat(detail, half, sizeof detail - strlen(detail) - 1);
    const bool ok = memcmp(first, STREAM_SAMPLE, 2) == 0 && memcmp(second, STREAM_SAMPLE, 2) == 0;
    report("raw read, no registers (0x40)", ok, detail);
}

/* 11. Four devices on one bus, each answering as itself.
 *
 * Ask each one the question only it answers: the two BMP280s their chip id,
 * the MPU-6050 its WHO_AM_I, the register-less device for its sample. The
 * emulator answers all four from one state machine, so a reply from the
 * wrong device, or none, means its address lookup is wrong.
 *
 * With a lone real BMP280, the other three are not there: SKIP. If only some
 * of them are, that is a FAIL — the emulator has all four or none. */
static void t_four_devices(void) {
    const char *name = "four devices, one bus";
    uint8_t first_id = 0, second_id = 0, mpu_id = 0;
    uint8_t sample[2] = {0};

    int rc = read_regs_at(TARGET_ADDR, REG_CHIP_ID, &first_id, 1);
    if (rc != 1) {
        report(name, false, i2c_err(rc, 1));
        return;
    }

    // The other three. Count the ones that are not there at all.
    int absent = 0;
    int rc_second = read_regs_at(SECOND_ADDR, REG_CHIP_ID, &second_id, 1);
    absent += rc_second == PICO_ERROR_GENERIC;
    int rc_mpu = read_regs_at(MPU_ADDR, MPU_WHO_AM_I, &mpu_id, 1);
    absent += rc_mpu == PICO_ERROR_GENERIC;
    int rc_stream = rig_read_at(STREAM_ADDR, sample, 2);
    absent += rc_stream == PICO_ERROR_GENERIC;
    if (absent == 3) {
        report_skip(name, "only 0x76 answers");
        return;
    }

    char detail[96];
    if (rc_second != 1) {
        g_addr = SECOND_ADDR;
        report(name, false, i2c_err(rc_second, 1));
        return;
    }
    if (rc_mpu != 1) {
        g_addr = MPU_ADDR;
        report(name, false, i2c_err(rc_mpu, 1));
        return;
    }
    if (rc_stream != 2) {
        g_addr = STREAM_ADDR;
        report(name, false, i2c_err(rc_stream, 2));
        return;
    }

    snprintf(detail, sizeof detail, "76: %02X  77: %02X  68: %02X  40: %02X %02X", first_id,
             second_id, mpu_id, sample[0], sample[1]);
    const bool ok = first_id == EXPECT_CHIP_ID && second_id == EXPECT_CHIP_ID &&
                    mpu_id == EXPECT_MPU_ID && memcmp(sample, STREAM_SAMPLE, 2) == 0;
    report(name, ok, detail);
}

/* 12. Devices on one bus keep their own state.
 *
 * Two BMP280s, one register map each, one pointer each. Point 0x76 at its
 * chip id and 0x77 at ctrl_meas, then read both with no pointer write: each
 * must still be where it was put. Then write ctrl_meas on 0x76 and check
 * 0x77's did not change. Shared state between devices — one pointer, or one
 * map — fails here and nowhere else. */
static void t_separate_state(void) {
    const char *name = "devices keep separate state";
    const uint8_t point_first = REG_CHIP_ID;
    const uint8_t point_second = REG_CTRL_MEAS;

    int rc = rig_write_at(SECOND_ADDR, &point_second, 1, false);
    if (rc == PICO_ERROR_GENERIC) {
        report_skip(name, "nothing at 0x77");
        return;
    }
    if (rc == 1) {
        rc = rig_write_at(TARGET_ADDR, &point_first, 1, false);
    }
    if (rc != 1) {
        report(name, false, i2c_err(rc, 1));
        return;
    }

    // Read 0x77 first: the write to 0x76 came after its pointer was set.
    uint8_t second_meas = 0xAA;
    uint8_t first_id = 0;
    rc = rig_read_at(SECOND_ADDR, &second_meas, 1);
    if (rc == 1) {
        rc = rig_read_at(TARGET_ADDR, &first_id, 1);
    }
    if (rc != 1) {
        report(name, false, i2c_err(rc, 1));
        return;
    }
    if (first_id != EXPECT_CHIP_ID || second_meas == EXPECT_CHIP_ID) {
        char detail[96];
        snprintf(detail, sizeof detail, "pointers: 0x76 read %02X (want %02X), 0x77 read %02X",
                 first_id, EXPECT_CHIP_ID, second_meas);
        report(name, false, detail);
        return;
    }

    // Now the maps. 0x77's ctrl_meas is what was just read; it must stay so.
    const uint8_t payload[2] = {REG_CTRL_MEAS, 0x27};
    uint8_t first_back = 0;
    uint8_t second_back = 0;
    int expected = 2;
    rc = rig_write_at(TARGET_ADDR, payload, 2, false);
    if (rc == expected) {
        expected = 1;
        rc = read_regs_at(TARGET_ADDR, REG_CTRL_MEAS, &first_back, 1);
        if (rc == expected) {
            rc = read_regs_at(SECOND_ADDR, REG_CTRL_MEAS, &second_back, 1);
        }
    }
    const uint8_t restore[2] = {REG_CTRL_MEAS, 0x00};
    const uint8_t failed_addr = g_addr;
    rig_write_at(TARGET_ADDR, restore, 2, false);
    if (rc != expected) {
        g_addr = failed_addr;
        report(name, false, i2c_err(rc, expected));
        return;
    }

    char detail[96];
    snprintf(detail, sizeof detail, "wrote 27 to 0x76: 0x76 reads %02X, 0x77 still %02X",
             first_back, second_back);
    report(name, first_back == 0x27 && second_back == second_meas, detail);
}

/* 7. Burst read across the end of the register map.
 *
 * Four bytes from 0xFE run past the last register. What comes back depends on
 * the target: a pointer that wraps reads 0xFE, 0xFF, 0x00, 0x01, one that
 * saturates reads 0xFE, 0xFF, 0xFF, 0xFF. Neither is wrong in general, so this
 * only checks the transfer completes and prints the bytes, to compare with
 * how the target is configured. */
static void t_burst_across_end(void) {
    uint8_t buf[4] = {0};
    int rc = read_regs(REG_END_OF_MAP, buf, (int)sizeof buf);
    if (rc != (int)sizeof buf) {
        report("burst across the end of the map", false, i2c_err(rc, (int)sizeof buf));
        return;
    }
    char detail[80];
    hex(detail, sizeof detail, buf, (int)sizeof buf);
    strncat(detail, " (from 0xFE: wrap or saturate?)", sizeof detail - strlen(detail) - 1);
    report("burst across the end of the map", true, detail);
}

/* 4. Register write, read back.
 *
 * ctrl_meas is writable on a BMP280. Writing 0 puts it in sleep mode, which
 * is safe and reversible. The read-back is the assertion: a slave that
 * accepts writes and discards them passes the write and fails here. */
static void t_register_write(void) {
    const uint8_t original_value = 0x00;
    uint8_t payload[2] = {REG_CTRL_MEAS, 0x27}; // oversampling x1, normal mode

    int rc = rig_write(payload, 2, false);
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
    rig_write(restore, 2, false);
}

/* 5. NACK handling.
 *
 * Addressing a device that is not there must fail, and must fail as a NACK
 * rather than a timeout. This is the one test that passes by failing, and it
 * proves the rig can tell "no device" from "device is slow" — a distinction
 * everything downstream depends on. */
static void t_nack_on_absent_address(void) {
    uint8_t v = 0;
    int rc = rig_read_at(ABSENT_ADDR, &v, 1);
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
    snprintf(detail, sizeof detail, "%lld us for a register read", elapsed_us);
    report("tolerates a stretched clock", rc == 1, rc == 1 ? detail : i2c_err(rc, 1));
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static uint g_baud = 400000;

static int run_once(bool verbose) {
    g_pass = 0;
    g_fail = 0;
    g_skip = 0;

    if (verbose) {
        printf("\nI2C master rig — target 0x%02X at %u kHz, SCL GP%d, SDA GP%d\n", TARGET_ADDR,
               g_baud / 1000, PIN_SCL, PIN_SDA);
        printf("----------------------------------------------------------------\n");
    }

    t_single_byte_read();
    t_pointer_then_repeated_start();
    t_burst_read();
    t_register_write();
    t_nack_on_absent_address();
    t_clock_stretch_tolerance();
    t_burst_across_end();
    t_read_only_write();
    t_burst_write();
    t_streaming_read();
    t_four_devices();
    t_separate_state();

    if (verbose) {
        printf("----------------------------------------------------------------\n");
        if (g_skip != 0) {
            printf("%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
        } else {
            printf("%d passed, %d failed\n", g_pass, g_fail);
        }
        if (g_fail == 0) {
            printf("ALL TRANSACTIONS CORRECT\n");
        }
    }
    return g_fail;
}

/* Continuous run for soak testing. One corrupted transaction in a million is
 * a real bug, not noise, so the counters are what matter here rather than the
 * per-test output.
 *
 * Runs the whole set over and over until SOAK_TARGET transactions, printing
 * only failures, and a progress line every few seconds. A skipped test means
 * part of the bus is missing, so a soak with skips does not pass either. */
#define SOAK_TARGET 1000000u
#define SOAK_PROGRESS_MS 5000u

static void soak(void) {
    uint32_t rounds = 0;
    uint32_t failures = 0;
    uint32_t skips = 0;
    bool stopped = false;

    g_transactions = 0;
    g_fails_shown = 0;
    g_quiet = true;
    printf("\nSoaking at %u kHz until %lu transactions. Any key stops it early.\n",
           g_baud / 1000, (unsigned long)SOAK_TARGET);

    const absolute_time_t start = get_absolute_time();
    absolute_time_t next_progress = delayed_by_ms(start, SOAK_PROGRESS_MS);
    while (g_transactions < SOAK_TARGET) {
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {
            stopped = true;
            break;
        }
        failures += (uint32_t)run_once(false);
        skips += (uint32_t)g_skip;
        rounds++;
        if (time_reached(next_progress)) {
            const uint32_t s = (uint32_t)(absolute_time_diff_us(start, get_absolute_time()) / 1000000);
            printf("  %7lu transactions, %lu failed, %lu s\n", (unsigned long)g_transactions,
                   (unsigned long)failures, (unsigned long)s);
            next_progress = delayed_by_ms(next_progress, SOAK_PROGRESS_MS);
        }
    }
    g_quiet = false;

    const uint64_t us = (uint64_t)absolute_time_diff_us(start, get_absolute_time());
    const uint32_t secs = (uint32_t)(us / 1000000u);
    const uint32_t rate = us ? (uint32_t)((uint64_t)g_transactions * 1000000u / us) : 0u;
    printf("\n%lu transactions in %lu rounds, %lu s (%lu per second) at %u kHz\n",
           (unsigned long)g_transactions, (unsigned long)rounds, (unsigned long)secs,
           (unsigned long)rate, g_baud / 1000);
    printf("%lu failed checks", (unsigned long)failures);
    if (failures > g_fails_shown) {
        printf(" (the first %lu shown above)", (unsigned long)g_fails_shown);
    }
    printf(", %lu skipped\n", (unsigned long)skips);
    if (stopped) {
        printf("SOAK STOPPED EARLY\n");
    } else if (failures == 0 && skips == 0) {
        printf("SOAK PASSED\n");
    } else {
        printf("SOAK FAILED\n");
    }
}

static void set_speed(uint baud) {
    g_baud = baud;
    uint nominal = i2c_set_baudrate(I2C_PORT, baud);
    // i2c_set_baudrate() returns what it aimed for. The real clock is a little
    // slower: measured 375 kHz at a 400 kHz setting.
    printf("Bus set to %u kHz (nominal; the real clock runs a little slower)\n",
           nominal / 1000);
}

static void help(void) {
    printf("\n  r  run once      s  soak: a million transactions, any key stops it\n");
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
                printf("\nEmuWire I2C master test rig — reconnected, %u kHz\n", g_baud / 1000);
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
