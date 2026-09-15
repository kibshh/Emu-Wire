/*
 * Bring-up harness for firmware/pio/i2c_slave.pio.
 *
 * Runs on the emulator Pico. It loads the PIO program, answers one fixed
 * address, and returns one fixed byte on every read — the narrowest thing
 * that can be checked against the master rig and a logic analyzer.
 *
 * This is scaffolding, not the product. The real firmware arrives with the
 * project skeleton; until then this exists so the PIO program can be run
 * against real silicon rather than only reasoned about.
 *
 *   Wiring        slave GP4 <-> rig GP4  (SDA)
 *                 slave GP5 <-> rig GP5  (SCL)
 *                 GND <-> GND
 *   Pull-ups      1 kOhm from each line to 3V3, once, anywhere on the bus.
 *                 There is no sensor breakout here to provide them.
 *
 * Serial output reports every address byte the state machine received, so a
 * transaction that never arrives looks different from one that arrives
 * wrong.
 */

#include <stdio.h>

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "i2c_slave.pio.h"

#define PIN_SDA 4 // SCL must be PIN_SDA + 1
#define BUS_HZ 400000

// The address this build answers to, and the byte it returns. A BMP280 at
// 0x76 returning its chip id, so the rig's existing expectations apply
// unchanged.
#define OUR_ADDR 0x76
#define OUR_BYTE 0x58

int main(void) {
    stdio_init_all();

    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2c_slave_program);
    i2c_slave_program_init(pio, sm, offset, PIN_SDA, BUS_HZ);

    // The data phase pulls from the TX FIFO. Pre-load it so the first read
    // does not stall waiting for the CPU — this version has no way to hold
    // the master off, which is exactly what the stretch handshake fixes.
    i2c_slave_prepare_byte(pio, sm, OUR_BYTE);

    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }

    printf("\nEmuWire PIO I2C slave — bring-up\n");
    printf("  program:  %d of 32 instructions, loaded at offset %u\n",
           i2c_slave_program.length, offset);
    printf("  pins:     SDA GP%d, SCL GP%d\n", PIN_SDA, PIN_SDA + 1);
    printf("  answers:  0x%02X, returns 0x%02X\n", OUR_ADDR, OUR_BYTE);
    printf("  NOTE:     this build ACKs every address. Matching comes next.\n\n");

    uint32_t count = 0;

    while (true) {
        // The address byte lands here left-aligned: autopush moved 8 bits
        // into a 32-bit word, so the byte sits in the top octet.
        uint32_t raw = pio_sm_get_blocking(pio, sm);
        uint8_t frame = (uint8_t)(raw >> 24);

        uint8_t addr = frame >> 1;
        bool is_read = frame & 1;
        count++;

        printf("  %6lu  addr 0x%02X %s%s\n", (unsigned long)count, addr,
               is_read ? "read " : "write",
               addr == OUR_ADDR ? "" : "   <- not our address, but ACKed anyway");

        // Reload for the next read. A real device decides this per
        // transaction; here it is the same byte every time.
        i2c_slave_prepare_byte(pio, sm, OUR_BYTE);
    }
}
