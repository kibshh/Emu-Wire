#include "bus/i2c_bus.h"

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#include "i2c_slave.pio.h"

/* Reserved by the I2C specification. A device here would answer general call,
 * CBUS, 10-bit addressing or device ID traffic. */
#define I2C_ADDR_RESERVED_LOW 0x07u  /* 0x00..0x07 */
#define I2C_ADDR_RESERVED_HIGH 0x78u /* 0x78..0x7F */

/* The IRQ handler has to find its bus with no argument, so one pointer is
 * kept here. One bus is all the first PIO slave supports; the bus manager
 * (which owns several) replaces this with a table indexed by state machine. */
static i2c_bus_t *s_bus;

/*
 * The handshake. The master is mid-transaction with SCL held low by the state
 * machine, so every path out of here must push an answer — including the
 * paths that report a problem. An early return with no answer stretches the
 * clock for ever and takes the bus down with it.
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
    pio_interrupt_clear(pio, sm);

    if (pio_sm_is_tx_fifo_full(pio, sm)) {
        /* Cannot answer. Recorded rather than ignored, because the symptom —
         * a bus stuck low — looks like dead hardware from the outside. */
        bus->tx_blocked++;
        return;
    }

    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        /* The program pushes the address before raising the IRQ, so this
         * cannot happen. NACK anyway: releasing the bus beats holding it. */
        bus->no_address++;
        pio_sm_put(pio, sm, I2C_SLAVE_NACK);
        return;
    }

    /* Autopush moved 8 bits into a 32-bit word, so the byte is left-aligned. */
    const uint8_t frame = (uint8_t)(pio_sm_get(pio, sm) >> 24);
    const uint8_t addr = (uint8_t)(frame >> 1);
    const bool is_read = (frame & 1u) != 0u;
    bus->last_frame = frame;

    const uint8_t slot = bus->index[addr]; /* O(1): one load, no scan */
    if (slot == I2C_BUS_NO_DEVICE) {
        bus->nacked_unknown++;
        pio_sm_put(pio, sm, I2C_SLAVE_NACK);
        return;
    }

    if (!is_read) {
        /* A real device ACKs a write. This one has nowhere to put the bytes
         * that would follow, and ACKing then ignoring them would be a lie the
         * master cannot see. Task 16 adds the write path. */
        bus->nacked_write++;
        pio_sm_put(pio, sm, I2C_SLAVE_NACK);
        return;
    }

    bus->acked++;
    pio_sm_put(pio, sm, I2C_SLAVE_ACK);
    i2c_slave_prepare_byte(pio, sm, bus->devices[slot].read_byte);
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
    bus->acked = 0;
    bus->nacked_unknown = 0;
    bus->nacked_write = 0;
    bus->no_address = 0;
    bus->tx_blocked = 0;
    bus->last_frame = 0;

    bus->pio = pio;
    bus->sm = sm;
    bus->pin_sda = pin_sda;
    bus->running = false;
    bus->offset = (uint)pio_add_program(pio, &i2c_slave_program);

    i2c_slave_program_init(pio, sm, bus->offset, pin_sda, bus_hz);

    s_bus = bus;
    return EMUWIRE_STATUS_OK;
}

emuwire_status_t i2c_bus_attach(i2c_bus_t *bus, uint8_t addr, uint8_t device_id,
                                uint8_t read_byte) {
    if (bus == NULL || addr >= I2C_BUS_ADDR_COUNT) {
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
    bus->devices[slot].read_byte = read_byte;

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

    /* Route this state machine's flag to the PIO's first system interrupt,
     * then enable it on the calling core. */
    pio_set_irq0_source_enabled(bus->pio, (pio_interrupt_source_t)(pis_interrupt0 + bus->sm),
                                true);
    irq_set_exclusive_handler((uint)irq, i2c_bus_handshake_isr);
    irq_set_enabled((uint)irq, true);

    bus->running = true;
    pio_sm_set_enabled(bus->pio, bus->sm, true);
    return EMUWIRE_STATUS_OK;
}
