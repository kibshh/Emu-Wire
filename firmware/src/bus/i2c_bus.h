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

/* Verbose builds count statistics and keep a transaction log. Quiet builds
 * (the default) compile all of it out: the IRQ handler answers the bus and
 * returns, with not even a flag to check. Failure counters are kept either
 * way — quiet means fewer messages, never hidden problems.
 *
 * Set at build time, e.g. -DI2C_BUS_VERBOSE=1. */
#ifndef I2C_BUS_VERBOSE
#define I2C_BUS_VERBOSE 0
#endif

/* The transaction log: one entry per decision, written by the IRQ handler
 * and read by the other core. Verbose builds only.
 *
 * One writer and one reader, no locks: the handler fills an entry, then
 * publishes it by advancing `log_head`; the reader consumes entries, then
 * advances `log_tail`. A full log drops new entries and counts them, so a
 * gap is always visible. Each entry is one word: event, byte, value. */
#define I2C_BUS_LOG_SIZE 64u /* power of two: the index is a mask */

typedef enum {
    I2C_LOG_ADDR_READ = 1,   /* address with read: ACKed */
    I2C_LOG_ADDR_WRITE,      /* address with write: ACKed */
    I2C_LOG_ADDR_UNKNOWN,    /* no device at the address: NACKed */
    I2C_LOG_DATA_POINTER,    /* first data byte: taken as the register pointer */
    I2C_LOG_DATA_WRITTEN,    /* a data byte stored: byte = the value, value = the register */
    I2C_LOG_DATA_IGNORED,    /* to a read-only register, ACKed and dropped like the real part */
    I2C_LOG_DATA_REFUSED,    /* to a read-only register, NACKed like the real part */
    I2C_LOG_DATA_NO_REGISTERS, /* to a streaming device, which has nothing to write to */
    I2C_LOG_DATA_STRAY,      /* a data byte outside any transaction: NACKed */
    I2C_LOG_TX_SENT,         /* a byte went out: byte = what the bus carried, value = its register */
    I2C_LOG_TX_MISMATCH,     /* the bus carried something else: byte = the bus, value = ours */
} i2c_log_event_t;

#define I2C_LOG_EVENT(e) ((uint8_t)((e) & 0xFFu))
#define I2C_LOG_BYTE(e) ((uint8_t)(((e) >> 8) & 0xFFu))
#define I2C_LOG_VALUE(e) ((uint8_t)(((e) >> 16) & 0xFFu))

typedef struct {
    uint8_t addr;      /* 7-bit, without the R/W bit */
    uint8_t device_id;

    /* The register map and one emuwire_register_flags byte per register, both
     * owned by the caller. The handler writes `regs` when the master writes a
     * register flagged WRITABLE. A NULL `reg_flags` makes the whole map
     * read-only. The full device model arrives with the regmap. */
    uint8_t *regs;
    const uint8_t *reg_flags;
    uint16_t reg_count;

    /* An emuwire_reg_addr_width_t. WIDTH_8: the master writes a register
     * number before it reads or writes. STREAMING: the device has no register
     * pointer at all; every read starts at the first byte of `regs` and runs
     * on, so array order is stream order, and a write has nowhere to go. */
    uint8_t reg_addr_width;

    /* Where the next read starts. The master sets it by writing one byte
     * after the address, which is how every register-addressed part works.
     * Written by the IRQ handler. */
    volatile uint8_t pointer;

    /* How the pointer moves after each byte: an emuwire_auto_increment_t.
     * ON_READ is what most sensors do. */
    volatile uint8_t auto_increment;

    /* emuwire_device_flags. WRAP decides the end of the map: set, the pointer
     * rolls to register 0; clear, it stays on the last register.
     * NACK_ON_RO_WRITE decides a write to a read-only register: set, it is
     * NACKed; clear, it is ACKed and dropped. Real parts do either. */
    volatile uint8_t flags;
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

    /* The transaction in progress. All owned by the IRQ handler. */
    volatile uint8_t active;     /* slot being addressed, or I2C_BUS_NO_DEVICE */
    volatile bool reading;       /* the master is reading from it */
    volatile uint8_t data_bytes; /* data bytes taken since the address */
    volatile uint8_t tx_reg;     /* the register of the byte being sent */
    volatile uint8_t tx_byte;    /* the byte being sent, to check its echo */

#if I2C_BUS_VERBOSE
    /* The transaction log — see I2C_BUS_LOG_SIZE. Written after the answer is
     * pushed, so it never lengthens a stretch. */
    volatile uint32_t log[I2C_BUS_LOG_SIZE];
    volatile uint32_t log_head;    /* next entry the handler writes */
    volatile uint32_t log_tail;    /* next entry the reader takes; the reader advances it */
    volatile uint32_t log_dropped; /* entries lost to a full log */
#endif

    /* Failures, counted in every build. Written by the IRQ handler only, read
     * by the other core; each is a single aligned word, so a reader never
     * sees half a value. */
    volatile uint32_t nacked_stray;    /* a data byte outside any transaction */
    volatile uint32_t no_address;      /* handshake IRQ with an empty RX FIFO */
    volatile uint32_t tx_blocked;      /* no room to answer: the bus will hang */
    volatile uint32_t tx_mismatch;     /* a byte sent read back different: something else drove SDA */
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

/* Everything that describes one device, for i2c_bus_attach(). The fields
 * mean what the same fields of i2c_device_t mean; the map and its flags stay
 * owned by the caller and must outlive the bus. */
typedef struct {
    uint8_t addr; /* 7-bit */
    uint8_t device_id;
    uint8_t *regs;
    const uint8_t *reg_flags;     /* may be NULL: all read-only */
    uint16_t reg_count;           /* 1..256 */
    uint8_t reg_addr_width;       /* emuwire_reg_addr_width_t: WIDTH_8 or STREAMING */
    uint8_t auto_increment;       /* emuwire_auto_increment_t; ignored when streaming */
    uint8_t flags;                /* emuwire_device_flags */
} i2c_device_config_t;

/* Attach a device. Rejects reserved addresses and an address that is
 * already taken: two devices at one address is a wiring fault that answers
 * with the bitwise AND of both, which is never what a test meant to set up.
 * Rejects WIDTH_16 too, for now: no 16-bit register addressing yet. */
emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, const i2c_device_config_t *config);

/* Install the handshake IRQ handler and start the state machine.
 *
 * CALL THIS FROM THE CORE THAT SHOULD SERVE THE BUS. Interrupt enables are
 * per-core, so whichever core calls this is the one that gets woken — core 1
 * by design, leaving core 0 to USB.
 */
emuwire_status_t i2c_bus_start(i2c_bus_t *bus);

#endif /* EMUWIRE_BUS_I2C_BUS_H */
