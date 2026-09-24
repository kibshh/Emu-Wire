/*
 * I2C bus — the CPU half of the clock-stretch handshake.
 *
 * The PIO state machine (firmware/pio/i2c_slave.pio) shifts one byte, holds
 * SCL low and raises an IRQ. The handler here decides what the byte means and
 * answers with one word: ACK or NACK, where the program continues, and, for a
 * read, the byte to send. Releasing the clock is what that answer does.
 *
 * So the protocol lives here, not in the program: address matching, the
 * register pointer, and which writes are accepted. Everything in this file
 * that runs from the IRQ is on the critical path of a held bus — the master
 * is waiting, in real time, for it to return.
 */
#ifndef EMUWIRE_BUS_I2C_BUS_H
#define EMUWIRE_BUS_I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#include "protocol/messages.h"

/* One state machine answers for the whole bus, so this is a limit of the
 * lookup, not of the hardware. */
#define I2C_BUS_MAX_DEVICES 8

/* 7-bit address space. The index below is a slot per address, so a lookup is
 * one load — never a scan over attached devices. */
#define I2C_BUS_ADDR_COUNT 128
#define I2C_BUS_NO_DEVICE 0xFFu

typedef struct {
    uint8_t addr;      /* 7-bit, without the R/W bit */
    uint8_t device_id;

    /* The register map, owned by the caller and never written here. The real
     * device model — writable registers, auto-increment, per-register flags —
     * arrives with the regmap; this is enough to answer a read. */
    const uint8_t *regs;
    uint16_t reg_count;

    /* Where the next read starts. The master sets it by writing one byte
     * after the address, which is how every register-addressed part works.
     * Written by the IRQ handler. */
    volatile uint8_t pointer;
} i2c_device_t;

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
    uint pin_sda;
    bool running;

    /* Where the program continues after each answer. Absolute addresses, so
     * the IRQ handler never has to add the load offset. */
    uint pc_idle;
    uint pc_rx;
    uint pc_tx;

    i2c_device_t devices[I2C_BUS_MAX_DEVICES];
    uint8_t device_count;
    uint8_t index[I2C_BUS_ADDR_COUNT]; /* address -> slot, or I2C_BUS_NO_DEVICE */

    /* The transaction in progress. Both are owned by the IRQ handler. */
    volatile uint8_t active;     /* slot being addressed, or I2C_BUS_NO_DEVICE */
    volatile uint8_t data_bytes; /* data bytes taken since the address */

    /* Written by the IRQ handler only, read by the other core. Each is a
     * single aligned word, so a reader never sees half a value, but a group
     * of them read together is not one instant. Diagnostics, not accounting. */
    volatile uint32_t acked;
    volatile uint32_t pointer_writes;  /* register pointer accepted */
    volatile uint32_t nacked_unknown;  /* no device at that address */
    volatile uint32_t nacked_write;    /* a write byte with nowhere to go */
    volatile uint32_t nacked_stray;    /* a data byte outside any transaction */
    volatile uint32_t no_address;      /* handshake IRQ with an empty RX FIFO */
    volatile uint32_t tx_blocked;      /* no room to answer: the bus will hang */
    volatile uint8_t last_frame;       /* last address byte seen, R/W bit included */
} i2c_bus_t;

/* Claim a state machine and load the program. SDA and SCL must be
 * consecutive pins, SCL = pin_sda + 1.
 *
 * The state machine is left stopped: it must not stretch the bus before
 * something can answer. i2c_bus_start() starts it.
 *
 * Returns ERR_BUSY if a bus is already initialised — for now there is one,
 * until the bus manager arrives.
 */
emuwire_status_t i2c_bus_init(i2c_bus_t *bus, PIO pio, uint sm, uint pin_sda, uint bus_hz);

/* Attach a device at a 7-bit address, backed by a register map the caller
 * owns and keeps alive. Rejects reserved addresses and an address that is
 * already taken: two devices at one address is a wiring fault that answers
 * with the bitwise AND of both, which is never what a test meant to set up.
 */
emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, uint8_t addr, uint8_t device_id,
                                const uint8_t *regs, uint16_t reg_count);

/* Install the handshake IRQ handler and start the state machine.
 *
 * CALL THIS FROM THE CORE THAT SHOULD SERVE THE BUS. Interrupt enables are
 * per-core, so whichever core calls this is the one that gets woken — core 1
 * by design, leaving core 0 to USB.
 */
emuwire_status_t i2c_bus_start(i2c_bus_t *bus);

#endif /* EMUWIRE_BUS_I2C_BUS_H */
