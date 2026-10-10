#include "bus/i2c_bus.h"

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#include "i2c_slave.pio.h"

/* Reserved by the I2C specification. A device here would answer general call,
 * CBUS, 10-bit addressing or device ID traffic. */
#define I2C_ADDR_RESERVED_LOW 0x07u  /* 0x00..0x07 */
#define I2C_ADDR_RESERVED_HIGH 0x78u /* 0x78..0x7F */

/* A read past the end of the register map. Real parts differ here — some
 * return 0x00, some the last value, some float — and the manifest will say
 * which. Until then, one obviously wrong value beats a plausible one. */
#define I2C_BUS_UNMAPPED_READ 0xFFu

/* The IRQ handler has to find its bus with no argument, so one pointer is
 * kept here. One bus is all the first PIO slave supports; the bus manager
 * (which owns several) replaces this with a table indexed by state machine. */
static i2c_bus_t *s_bus;

#if I2C_BUS_VERBOSE
/* Append one entry to the transaction log, or count it as dropped. Inlined
 * into the handler, so it runs from RAM with it. */
static __force_inline void i2c_bus_log(i2c_bus_t *bus, i2c_log_event_t event, uint8_t byte,
                                       uint8_t value) {
    const uint32_t head = bus->log_head;
    if (head - bus->log_tail >= I2C_BUS_LOG_SIZE) {
        bus->log_dropped++;
        return;
    }
    bus->log[head & (I2C_BUS_LOG_SIZE - 1u)] =
        (uint32_t)event | ((uint32_t)byte << 8) | ((uint32_t)value << 16);
    /* The entry must be visible to the other core before the head says so. */
    __dmb();
    bus->log_head = head + 1u;
}
#endif

static inline uint8_t i2c_device_read(const i2c_device_t *dev, uint8_t reg) {
    return (reg < dev->reg_count) ? dev->regs[reg] : I2C_BUS_UNMAPPED_READ;
}

/* The register after `reg`, for a byte just read or written. Whether the
 * pointer moves at all depends on the device's auto-increment mode; at the
 * end of the map it rolls to 0 with WRAP, and otherwise stays put. */
static inline uint8_t i2c_device_next(const i2c_device_t *dev, uint8_t reg, bool was_read) {
    const uint8_t mode = dev->auto_increment;
    const bool moves = (mode == EMUWIRE_AUTO_INCREMENT_BOTH) ||
                       (mode == (was_read ? EMUWIRE_AUTO_INCREMENT_ON_READ
                                          : EMUWIRE_AUTO_INCREMENT_ON_WRITE));
    if (!moves) {
        return reg;
    }
    if ((uint32_t)reg + 1u < dev->reg_count) {
        return (uint8_t)(reg + 1u);
    }
    return (dev->flags & EMUWIRE_DEVICE_FLAGS_WRAP) ? 0u : reg;
}

/*
 * One byte, one decision.
 *
 * The master is mid-transaction with SCL held low by the state machine, so
 * every path out of here must push an answer — including the paths that
 * report a problem. An early return with no answer stretches the clock for
 * ever and takes the bus down with it.
 *
 * Runs from RAM: the first call after a flash read would otherwise pay for an
 * XIP cache miss while the bus is held.
 */
static void __not_in_flash_func(i2c_bus_handshake_isr)(void) {
    i2c_bus_t *bus = s_bus;
    if (bus == NULL) {
        return;
    }

    PIO pio = bus->pio;
    const uint sm = bus->sm;

    /* "irq nowait 0 rel" raises the flag whose number is the state machine's,
     * so the flag index and sm are the same. */
    if (!pio_interrupt_get(pio, sm)) {
        return;
    }

    /* Flag 4 + sm is not an interrupt; it is a label set at every START. Set
     * means this byte is an address. Clear means it belongs to the
     * transaction in progress: data the master wrote, or the echo of a byte
     * we sent — `reading` says which. */
    const uint address_flag = I2C_SLAVE_MARK_ADDRESS + sm;
    const bool is_address = pio_interrupt_get(pio, address_flag);
    if (is_address) {
        pio_interrupt_clear(pio, address_flag);
    }
    pio_interrupt_clear(pio, sm);

    if (pio_sm_is_tx_fifo_full(pio, sm)) {
        /* Cannot answer. Recorded rather than ignored, because the symptom —
         * a bus stuck low — looks like dead hardware from the outside. */
        bus->tx_blocked++;
        return;
    }

    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        /* Every handshake follows eight bits and an autopush, so this cannot
         * happen. NACK anyway: releasing the bus beats holding it. */
        bus->no_address++;
        bus->active = I2C_BUS_NO_DEVICE;
        pio_sm_put(pio, sm, i2c_slave_answer(false, bus->pc_idle));
        return;
    }

    /* IN shifts left: each new bit enters at bit 0 and pushes the older ones
     * up. After eight, the byte is in bits 7:0, and autopush hands the word
     * over as it is. (OUT is the mirror image: shifting left, it sends from
     * bit 31 down, which is why the answer word is built from the top.)
     * Found on the bench: reading bits 31:24 gave 0x00 for every address. */
    const uint8_t byte = (uint8_t)pio_sm_get(pio, sm);

    /* Decide first. Every branch sets the answer and what to log, and nothing
     * here is diagnostic: only state the bus depends on is touched. */
    uint32_t answer;
    i2c_log_event_t event;
    uint8_t value = 0;

    if (is_address) {
        const uint8_t addr = (uint8_t)(byte >> 1);
        const uint8_t slot = bus->index[addr]; /* O(1): one load, no scan */
        if (slot == I2C_BUS_NO_DEVICE) {
            bus->active = I2C_BUS_NO_DEVICE;
            answer = i2c_slave_answer(false, bus->pc_idle);
            event = I2C_LOG_ADDR_UNKNOWN;
        } else {
            i2c_device_t *dev = &bus->devices[slot];
            bus->active = slot;
            bus->data_bytes = 0;
            bus->reading = (byte & 1u) != 0u;
            if (bus->reading) {
                /* Read from wherever the pointer was left — by a pointer write
                 * just before this, or by the transaction before that. A part
                 * that has never been pointed anywhere reads register 0. The
                 * pointer only moves once the byte has really gone out. */
                bus->tx_reg = dev->pointer;
                bus->tx_byte = i2c_device_read(dev, bus->tx_reg);
                answer = i2c_slave_answer_send(true, bus->pc_tx, bus->tx_byte);
                event = I2C_LOG_ADDR_READ;
            } else {
                answer = i2c_slave_answer(true, bus->pc_rx);
                event = I2C_LOG_ADDR_WRITE;
            }
        }
    } else if (bus->active == I2C_BUS_NO_DEVICE) {
        /* No transaction owns this byte. A START always marks the next byte
         * as an address, so this needs a master that ignored a NACK. Counted
         * so it is visible rather than mysterious. */
        bus->nacked_stray++;
        answer = i2c_slave_answer(false, bus->pc_idle);
        event = I2C_LOG_DATA_STRAY;
    } else if (bus->reading) {
        /* The echo of a byte we sent: what was actually on the bus. It went
         * out, so the pointer moves past it now. Then offer the next one; the
         * ACK bit is clear, so the master answers on the ninth clock — ACK for
         * more, NACK to stop — and the program acts on that by itself. A
         * byte offered and not taken never moved the pointer. */
        i2c_device_t *dev = &bus->devices[bus->active];
        if (byte != bus->tx_byte) {
            bus->tx_mismatch++;
            event = I2C_LOG_TX_MISMATCH;
            value = bus->tx_byte;
        } else {
            event = I2C_LOG_TX_SENT;
            value = bus->tx_reg;
        }
        dev->pointer = i2c_device_next(dev, bus->tx_reg, true);
        bus->tx_reg = dev->pointer;
        bus->tx_byte = i2c_device_read(dev, bus->tx_reg);
        answer = i2c_slave_answer_send(false, bus->pc_tx, bus->tx_byte);
    } else if (bus->data_bytes == 0 ||
               ((bus->devices[bus->active].flags & EMUWIRE_DEVICE_FLAGS_WRITE_PAIRS) &&
                (bus->data_bytes & 1u) == 0u)) {
        /* A register number. The first byte after a write address always is
         * one; with WRITE_PAIRS, so is every byte after a value: the master
         * writes reg, val, reg, val. */
        bus->devices[bus->active].pointer = byte;
        bus->data_bytes++;
        answer = i2c_slave_answer(true, bus->pc_rx);
        event = I2C_LOG_DATA_POINTER;
    } else {
        /* A value for the register at the pointer. */
        i2c_device_t *dev = &bus->devices[bus->active];
        const uint8_t reg = dev->pointer;
        bus->data_bytes++;
        const bool writable = reg < dev->reg_count && dev->reg_flags != NULL &&
                              (dev->reg_flags[reg] & EMUWIRE_REGISTER_FLAGS_WRITABLE);
        value = reg;
        if (writable) {
            dev->regs[reg] = byte;
            dev->pointer = i2c_device_next(dev, reg, false);
            answer = i2c_slave_answer(true, bus->pc_rx);
            event = I2C_LOG_DATA_WRITTEN;
        } else if (dev->flags & EMUWIRE_DEVICE_FLAGS_NACK_ON_RO_WRITE) {
            /* This part refuses: the master sees a NACK and the transaction
             * ends. */
            bus->active = I2C_BUS_NO_DEVICE;
            answer = i2c_slave_answer(false, bus->pc_idle);
            event = I2C_LOG_DATA_REFUSED;
        } else {
            /* This part takes the byte and drops it, and its pointer moves on
             * as if it had been stored. Not a lie to the master: it is what
             * the real part does, and the log says so. */
            dev->pointer = i2c_device_next(dev, reg, false);
            answer = i2c_slave_answer(true, bus->pc_rx);
            event = I2C_LOG_DATA_IGNORED;
        }
    }

    /* Answer, and the master moves on. Everything after this line is after
     * the clock has been released, so none of it can lengthen a stretch. */
    pio_sm_put(pio, sm, answer);

#if I2C_BUS_VERBOSE
    i2c_bus_log(bus, event, byte, value);
#else
    (void)event; /* only the log needs them */
    (void)value;
#endif
}

emuwire_status_t i2c_bus_init(i2c_bus_t *bus, PIO pio, uint sm, uint pin_sda, uint bus_hz) {
    if (bus == NULL || sm >= NUM_PIO_STATE_MACHINES || bus_hz == 0u) {
        return EMUWIRE_STATUS_ERR_BAD_PARAM;
    }
    /* SCL is SDA + 1 and both must exist on the package. */
    if (pin_sda + 1u >= NUM_BANK0_GPIOS) {
        return EMUWIRE_STATUS_ERR_PIN_UNAVAILABLE;
    }
    if (s_bus != NULL) {
        return EMUWIRE_STATUS_ERR_BUSY;
    }
    if (!pio_can_add_program(pio, &i2c_slave_program)) {
        return EMUWIRE_STATUS_ERR_PIO_PROGRAM_SPACE;
    }

    for (uint i = 0; i < I2C_BUS_ADDR_COUNT; i++) {
        bus->index[i] = I2C_BUS_NO_DEVICE;
    }
    bus->device_count = 0;
    bus->active = I2C_BUS_NO_DEVICE;
    bus->reading = false;
    bus->data_bytes = 0;
#if I2C_BUS_VERBOSE
    bus->log_head = 0;
    bus->log_tail = 0;
    bus->log_dropped = 0;
#endif
    bus->nacked_stray = 0;
    bus->no_address = 0;
    bus->tx_blocked = 0;
    bus->tx_mismatch = 0;

    bus->pio = pio;
    bus->sm = sm;
    bus->pin_sda = pin_sda;
    bus->running = false;
    bus->offset = (uint)pio_add_program(pio, &i2c_slave_program);

    /* The answers carry absolute addresses, so resolve them once here rather
     * than adding the offset in the IRQ path. */
    bus->pc_idle = bus->offset + i2c_slave_offset_idle;
    bus->pc_rx = bus->offset + i2c_slave_offset_rx_path;
    bus->pc_tx = bus->offset + i2c_slave_offset_byte_loop;

    i2c_slave_program_init(pio, sm, bus->offset, pin_sda, bus_hz);

    s_bus = bus;
    return EMUWIRE_STATUS_OK;
}

emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, uint8_t addr, uint8_t device_id,
                                uint8_t *regs, const uint8_t *reg_flags, uint16_t reg_count,
                                uint8_t auto_increment, uint8_t flags) {
    if (bus == NULL || addr >= I2C_BUS_ADDR_COUNT || regs == NULL || reg_count == 0u ||
        reg_count > 256u || auto_increment > EMUWIRE_AUTO_INCREMENT_BOTH) {
        return EMUWIRE_STATUS_ERR_BAD_PARAM;
    }
    if (addr <= I2C_ADDR_RESERVED_LOW || addr >= I2C_ADDR_RESERVED_HIGH) {
        return EMUWIRE_STATUS_ERR_ADDRESS_RESERVED;
    }
    if (bus->index[addr] != I2C_BUS_NO_DEVICE) {
        return EMUWIRE_STATUS_ERR_ADDRESS_IN_USE;
    }
    if (bus->device_count >= I2C_BUS_MAX_DEVICES) {
        return EMUWIRE_STATUS_ERR_DEVICE_LIMIT;
    }

    const uint8_t slot = bus->device_count;
    bus->devices[slot].addr = addr;
    bus->devices[slot].device_id = device_id;
    bus->devices[slot].regs = regs;
    bus->devices[slot].reg_flags = reg_flags;
    bus->devices[slot].reg_count = reg_count;
    bus->devices[slot].pointer = 0;
    bus->devices[slot].auto_increment = auto_increment;
    bus->devices[slot].flags = flags;

    /* Published last: the IRQ handler reads the index, so the slot it points
     * at must already be complete. */
    __dmb();
    bus->index[addr] = slot;
    bus->device_count = (uint8_t)(slot + 1u);
    return EMUWIRE_STATUS_OK;
}

emuwire_status_t i2c_bus_start(i2c_bus_t *bus) {
    if (bus == NULL || bus != s_bus) {
        return EMUWIRE_STATUS_ERR_BAD_PARAM;
    }
    if (bus->running) {
        return EMUWIRE_STATUS_ERR_BUSY;
    }

    const int irq = pio_get_irq_num(bus->pio, 0);
    if (irq < 0) {
        return EMUWIRE_STATUS_ERR_INTERNAL;
    }

    /* Only the handshake flag reaches the CPU. The address marker lives in
     * flags 4-7, which cannot raise an interrupt, and is read inside the
     * handler. */
    pio_set_irq0_source_enabled(bus->pio, (pio_interrupt_source_t)(pis_interrupt0 + bus->sm),
                                true);
    irq_set_exclusive_handler((uint)irq, i2c_bus_handshake_isr);
    irq_set_enabled((uint)irq, true);

    bus->running = true;
    pio_sm_set_enabled(bus->pio, bus->sm, true);
    return EMUWIRE_STATUS_OK;
}
