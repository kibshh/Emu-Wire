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

static inline uint8_t i2c_device_read(const i2c_device_t *dev) {
    const uint8_t pointer = dev->pointer;
    return (pointer < dev->reg_count) ? dev->regs[pointer] : I2C_BUS_UNMAPPED_READ;
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

    /* Flag 4 + sm is not an interrupt; it is a label the receive path puts on
     * the byte. Set means data, clear means an address — including after a
     * repeated START, which the program handles without telling anyone. */
    const uint data_flag = I2C_SLAVE_MARK_DATA + sm;
    const bool is_data = pio_interrupt_get(pio, data_flag);
    if (is_data) {
        pio_interrupt_clear(pio, data_flag);
    }
    pio_interrupt_clear(pio, sm);

    if (pio_sm_is_tx_fifo_full(pio, sm)) {
        /* Cannot answer. Recorded rather than ignored, because the symptom —
         * a bus stuck low — looks like dead hardware from the outside. */
        bus->tx_blocked++;
        return;
    }

    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        /* The program pushes the byte before raising the IRQ, so this cannot
         * happen. NACK anyway: releasing the bus beats holding it. */
        bus->no_address++;
        bus->active = I2C_BUS_NO_DEVICE;
        pio_sm_put(pio, sm, i2c_slave_answer(false, bus->pc_idle, 0));
        return;
    }

    /* Autopush moved 8 bits into a 32-bit word, so the byte is left-aligned. */
    const uint8_t byte = (uint8_t)(pio_sm_get(pio, sm) >> 24);

    if (!is_data) {
        const uint8_t addr = (uint8_t)(byte >> 1);
        const bool is_read = (byte & 1u) != 0u;
        bus->last_frame = byte;

        const uint8_t slot = bus->index[addr]; /* O(1): one load, no scan */
        if (slot == I2C_BUS_NO_DEVICE) {
            bus->active = I2C_BUS_NO_DEVICE;
            bus->nacked_unknown++;
            pio_sm_put(pio, sm, i2c_slave_answer(false, bus->pc_idle, 0));
            return;
        }

        bus->active = slot;
        bus->data_bytes = 0;
        bus->acked++;

        if (is_read) {
            /* Read from wherever the pointer was left — by a pointer write
             * just before this, or by the transaction before that. A part
             * that has never been pointed anywhere reads register 0. */
            pio_sm_put(pio, sm,
                       i2c_slave_answer(true, bus->pc_tx, i2c_device_read(&bus->devices[slot])));
        } else {
            pio_sm_put(pio, sm, i2c_slave_answer(true, bus->pc_rx, 0));
        }
        return;
    }

    /* A data byte: the master is writing. */
    const uint8_t slot = bus->active;
    if (slot == I2C_BUS_NO_DEVICE) {
        /* No transaction owns this byte. Reachable only if a byte was cut
         * short mid-flight, leaving the marker set; the next address recovers
         * it. Counted so a run of these is visible rather than mysterious. */
        bus->nacked_stray++;
        pio_sm_put(pio, sm, i2c_slave_answer(false, bus->pc_idle, 0));
        return;
    }

    if (bus->data_bytes == 0) {
        /* The first byte after a write address is the register pointer. */
        bus->devices[slot].pointer = byte;
        bus->data_bytes = 1;
        bus->pointer_writes++;
        pio_sm_put(pio, sm, i2c_slave_answer(true, bus->pc_rx, 0));
        return;
    }

    /* Anything after it would be a register write, and there is nowhere to
     * put it: the register map is read-only here. ACKing and discarding would
     * be a lie the master cannot detect, so NACK and let it fail honestly. */
    bus->nacked_write++;
    bus->active = I2C_BUS_NO_DEVICE;
    pio_sm_put(pio, sm, i2c_slave_answer(false, bus->pc_idle, 0));
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
    bus->data_bytes = 0;
    bus->acked = 0;
    bus->pointer_writes = 0;
    bus->nacked_unknown = 0;
    bus->nacked_write = 0;
    bus->nacked_stray = 0;
    bus->no_address = 0;
    bus->tx_blocked = 0;
    bus->last_frame = 0;

    bus->pio = pio;
    bus->sm = sm;
    bus->pin_sda = pin_sda;
    bus->running = false;
    bus->offset = (uint)pio_add_program(pio, &i2c_slave_program);

    /* The answers carry absolute addresses, so resolve them once here rather
     * than adding the offset in the IRQ path. */
    bus->pc_idle = bus->offset + i2c_slave_offset_idle;
    bus->pc_rx = bus->offset + i2c_slave_offset_rx_path;
    bus->pc_tx = bus->offset + i2c_slave_offset_tx_path;

    i2c_slave_program_init(pio, sm, bus->offset, pin_sda, bus_hz);

    s_bus = bus;
    return EMUWIRE_STATUS_OK;
}

emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, uint8_t addr, uint8_t device_id,
                                const uint8_t *regs, uint16_t reg_count) {
    if (bus == NULL || addr >= I2C_BUS_ADDR_COUNT || regs == NULL || reg_count == 0u) {
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
    bus->devices[slot].reg_count = reg_count;
    bus->devices[slot].pointer = 0;

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

    /* Only the handshake flag reaches the CPU. The data marker lives in flags
     * 4-7, which cannot raise an interrupt, and is read inside the handler. */
    pio_set_irq0_source_enabled(bus->pio, (pio_interrupt_source_t)(pis_interrupt0 + bus->sm),
                                true);
    irq_set_exclusive_handler((uint)irq, i2c_bus_handshake_isr);
    irq_set_enabled((uint)irq, true);

    bus->running = true;
    pio_sm_set_enabled(bus->pio, bus->sm, true);
    return EMUWIRE_STATUS_OK;
}
