/*
 * I2C bus — the CPU half of the clock-stretch ACK handshake.
 *
 * The PIO state machine (firmware/pio/i2c_slave.pio) shifts in an address
 * byte, holds SCL low and raises an IRQ. The handler here looks the address
 * up and pushes back ACK or NACK, which releases the clock. Everything in
 * this file that runs from the IRQ is on the critical path of a held bus: the
 * master is waiting, in real time, for it to return.
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
    /* The byte every read returns. A placeholder for the register model,
     * which arrives in task 14 and replaces this field. */
    uint8_t read_byte;
} i2c_device_t;

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
    uint pin_sda;
    bool running;

    i2c_device_t devices[I2C_BUS_MAX_DEVICES];
    uint8_t device_count;
    uint8_t index[I2C_BUS_ADDR_COUNT]; /* address -> slot, or I2C_BUS_NO_DEVICE */

    /* Written by the IRQ handler only, read by the other core. Each is a
     * single aligned word, so a reader never sees half a value, but a group
     * of them read together is not one instant. Diagnostics, not accounting. */
    volatile uint32_t acked;
    volatile uint32_t nacked_unknown; /* no device at that address */
    volatile uint32_t nacked_write;   /* no write path yet — see task 16 */
    volatile uint32_t no_address;     /* handshake IRQ with an empty RX FIFO */
    volatile uint32_t tx_blocked;     /* no room to answer: the bus will hang */
    volatile uint8_t last_frame;      /* last address byte seen, R/W bit included */
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

/* Attach a device at a 7-bit address. Rejects reserved addresses and an
 * address that is already taken: two devices at one address is a wiring fault
 * that answers with the bitwise AND of both, which is never what a test
 * meant to set up. */
emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, uint8_t addr, uint8_t device_id,
                                uint8_t read_byte);

/* Install the handshake IRQ handler and start the state machine.
 *
 * CALL THIS FROM THE CORE THAT SHOULD SERVE THE BUS. Interrupt enables are
 * per-core, so whichever core calls this is the one that gets woken — core 1
 * by design, leaving core 0 to USB.
 */
emuwire_status_t i2c_bus_start(i2c_bus_t *bus);

#endif /* EMUWIRE_BUS_I2C_BUS_H */
