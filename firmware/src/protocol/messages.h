/*
 * DO NOT EDIT — generated from protocol/protocol.yaml by protocol/codegen.py
 *
 * Hand-editing this file makes the firmware and the host SDK disagree about
 * the wire format, and CI fails on the difference. Change the spec under
 * protocol/ and re-run:  python protocol/codegen.py
 */


#ifndef EMUWIRE_MESSAGES_H
#define EMUWIRE_MESSAGES_H

#include <stdint.h>
#include <stdbool.h>

/* ---- protocol ---- */
#define EMUWIRE_PROTOCOL_VERSION 1u

/* ---- frame ---- */
#define EMUWIRE_FRAME_MAGIC 0xa5u
#define EMUWIRE_FRAME_HEADER_BYTES 5u
#define EMUWIRE_FRAME_TRAILER_BYTES 2u
#define EMUWIRE_FRAME_OVERHEAD_BYTES 7u
/* Validate LEN against this BEFORE reading a payload. */
#define EMUWIRE_MAX_PAYLOAD_BYTES 8192u
#define EMUWIRE_SEQ_ASYNC 0u

/* ---- CRC: CRC-16/IBM-3740 ---- */
#define EMUWIRE_CRC_POLY 0x1021u
#define EMUWIRE_CRC_INIT 0xffffu
#define EMUWIRE_CRC_XOROUT 0x0000u
#define EMUWIRE_CRC_REFLECT_IN false
#define EMUWIRE_CRC_REFLECT_OUT false
/* CRC of "123456789". crc16.c must reproduce this exactly. */
#define EMUWIRE_CRC_CHECK 0x29b1u

/* ---- named sentinels ---- */
#define EMUWIRE_PIN_UNUSED ((uint8_t)0xff)
#define EMUWIRE_DEV_ID_ALL ((uint8_t)0xff)
#define EMUWIRE_FAULT_ID_ALL ((uint8_t)0xff)
#define EMUWIRE_DEV_ID_NONE ((uint8_t)0xff)
#define EMUWIRE_MSG_TYPE_NONE ((uint8_t)0x0)
#define EMUWIRE_BUS_ID_NONE ((uint8_t)0xff)
#define EMUWIRE_REG_ANY ((uint16_t)0xffff)
#define EMUWIRE_FAULT_REPEAT_UNLIMITED ((uint16_t)0xffff)

/* ---- enums ---- */
typedef enum {
    EMUWIRE_STATUS_OK = 0x00,
    EMUWIRE_STATUS_ERR_BAD_LENGTH = 0x01,
    EMUWIRE_STATUS_ERR_BAD_CRC = 0x02,
    EMUWIRE_STATUS_ERR_UNKNOWN_TYPE = 0x03,
    EMUWIRE_STATUS_ERR_BAD_PARAM = 0x04,
    EMUWIRE_STATUS_ERR_NO_SUCH_BUS = 0x05,
    EMUWIRE_STATUS_ERR_NO_SUCH_DEVICE = 0x06,
    EMUWIRE_STATUS_ERR_ADDRESS_IN_USE = 0x07,
    EMUWIRE_STATUS_ERR_ADDRESS_RESERVED = 0x08,
    EMUWIRE_STATUS_ERR_BUS_LIMIT = 0x09,
    EMUWIRE_STATUS_ERR_DEVICE_LIMIT = 0x0a,
    EMUWIRE_STATUS_ERR_NO_FREE_SM = 0x0b,
    EMUWIRE_STATUS_ERR_PIO_PROGRAM_SPACE = 0x0c,
    EMUWIRE_STATUS_ERR_PIN_UNAVAILABLE = 0x0d,
    EMUWIRE_STATUS_ERR_PIN_IN_USE = 0x0e,
    EMUWIRE_STATUS_ERR_UNSUPPORTED_PROTOCOL = 0x0f,
    EMUWIRE_STATUS_ERR_REG_OUT_OF_RANGE = 0x10,
    EMUWIRE_STATUS_ERR_REG_READ_ONLY = 0x11,
    EMUWIRE_STATUS_ERR_NO_SUCH_FAULT = 0x12,
    EMUWIRE_STATUS_ERR_FAULT_LIMIT = 0x13,
    EMUWIRE_STATUS_ERR_UNSUPPORTED_FAULT = 0x14,
    EMUWIRE_STATUS_ERR_TRACE_ACTIVE = 0x15,
    EMUWIRE_STATUS_ERR_TRACE_NOT_ACTIVE = 0x16,
    EMUWIRE_STATUS_ERR_TRACE_OVERFLOW = 0x17,
    EMUWIRE_STATUS_ERR_CLOCK_OUT_OF_SPEC = 0x18,
    EMUWIRE_STATUS_ERR_BUSY = 0x19,
    EMUWIRE_STATUS_ERR_NOT_IMPLEMENTED = 0x1a,
    EMUWIRE_STATUS_ERR_INTERNAL = 0x1b,
} emuwire_status_t;

typedef enum {
    EMUWIRE_ADDRESS_MODE_ADDR_7BIT = 0x00,
    EMUWIRE_ADDRESS_MODE_ADDR_10BIT = 0x01,
    EMUWIRE_ADDRESS_MODE_ADDR_CHIP_SELECT = 0x02,
} emuwire_address_mode_t;

typedef enum {
    EMUWIRE_REG_ADDR_WIDTH_STREAMING = 0x00,
    EMUWIRE_REG_ADDR_WIDTH_WIDTH_8 = 0x01,
    EMUWIRE_REG_ADDR_WIDTH_WIDTH_16 = 0x02,
} emuwire_reg_addr_width_t;

typedef enum {
    EMUWIRE_AUTO_INCREMENT_NONE = 0x00,
    EMUWIRE_AUTO_INCREMENT_ON_READ = 0x01,
    EMUWIRE_AUTO_INCREMENT_ON_WRITE = 0x02,
    EMUWIRE_AUTO_INCREMENT_BOTH = 0x03,
} emuwire_auto_increment_t;

typedef enum {
    EMUWIRE_ACCESS_KIND_READ = 0x00,
    EMUWIRE_ACCESS_KIND_WRITE = 0x01,
} emuwire_access_kind_t;

typedef enum {
    EMUWIRE_FAULT_TYPE_NACK_ON_READ = 0x01,
    EMUWIRE_FAULT_TYPE_NACK_ON_WRITE = 0x02,
    EMUWIRE_FAULT_TYPE_BUS_HANG = 0x03,
    EMUWIRE_FAULT_TYPE_CORRUPT_REGISTER = 0x04,
    EMUWIRE_FAULT_TYPE_OUT_OF_RANGE = 0x05,
    EMUWIRE_FAULT_TYPE_STUCK_VALUE = 0x06,
    EMUWIRE_FAULT_TYPE_CLOCK_STRETCH = 0x07,
    EMUWIRE_FAULT_TYPE_SETUP_VIOLATION = 0x40,
    EMUWIRE_FAULT_TYPE_HOLD_VIOLATION = 0x41,
    EMUWIRE_FAULT_TYPE_PARTIAL_BYTE = 0x42,
    EMUWIRE_FAULT_TYPE_ACK_GLITCH = 0x43,
    EMUWIRE_FAULT_TYPE_CLOCK_SKEW = 0x44,
} emuwire_fault_type_t;

typedef enum {
    EMUWIRE_FAULT_TRIGGER_IMMEDIATE = 0x00,
    EMUWIRE_FAULT_TRIGGER_ON_NTH_ACCESS = 0x01,
    EMUWIRE_FAULT_TRIGGER_RANDOM_ACCESS = 0x02,
    EMUWIRE_FAULT_TRIGGER_AFTER_MS = 0x03,
    EMUWIRE_FAULT_TRIGGER_RANDOM_MS = 0x04,
} emuwire_fault_trigger_t;

typedef enum {
    EMUWIRE_ACCESS_FILTER_ACCESS_READ = 0x00,
    EMUWIRE_ACCESS_FILTER_ACCESS_WRITE = 0x01,
    EMUWIRE_ACCESS_FILTER_ACCESS_ANY = 0x02,
} emuwire_access_filter_t;

typedef enum {
    EMUWIRE_BOARD_ID_UNKNOWN = 0x00,
    EMUWIRE_BOARD_ID_PICO2 = 0x01,
    EMUWIRE_BOARD_ID_PICO2W = 0x02,
    EMUWIRE_BOARD_ID_CUSTOM = 0xff,
} emuwire_board_id_t;

typedef enum {
    EMUWIRE_MCU_ID_RP2350A = 0x01,
    EMUWIRE_MCU_ID_RP2350B = 0x02,
} emuwire_mcu_id_t;

typedef enum {
    EMUWIRE_RESET_MODE_SOFT = 0x00,
    EMUWIRE_RESET_MODE_BOOTSEL = 0x01,
} emuwire_reset_mode_t;

typedef enum {  /* derived from buses/ */
    EMUWIRE_BUS_PROTOCOL_I2C = 0x01,
    EMUWIRE_BUS_PROTOCOL_SPI = 0x02,
} emuwire_bus_protocol_t;

typedef enum {  /* derived from messages/ */
    EMUWIRE_MSG_TYPE_PING = 0x01,
    EMUWIRE_MSG_TYPE_INFO = 0x02,
    EMUWIRE_MSG_TYPE_RESET = 0x03,
    EMUWIRE_MSG_TYPE_BUS_CREATE = 0x10,
    EMUWIRE_MSG_TYPE_BUS_DESTROY = 0x11,
    EMUWIRE_MSG_TYPE_BUS_LIST = 0x12,
    EMUWIRE_MSG_TYPE_DEV_ATTACH = 0x20,
    EMUWIRE_MSG_TYPE_DEV_DETACH = 0x21,
    EMUWIRE_MSG_TYPE_DEV_WRITE_REGS = 0x22,
    EMUWIRE_MSG_TYPE_DEV_READ_REGS = 0x23,
    EMUWIRE_MSG_TYPE_FAULT_SET = 0x30,
    EMUWIRE_MSG_TYPE_FAULT_CLEAR = 0x31,
    EMUWIRE_MSG_TYPE_FAULT_LIST = 0x32,
    EMUWIRE_MSG_TYPE_TRACE_START = 0x40,
    EMUWIRE_MSG_TYPE_TRACE_STOP = 0x41,
    EMUWIRE_MSG_TYPE_EVT_DEV_ACCESS = 0x80,
    EMUWIRE_MSG_TYPE_EVT_FAULT_FIRED = 0x81,
    EMUWIRE_MSG_TYPE_EVT_TRACE_DATA = 0x82,
    EMUWIRE_MSG_TYPE_EVT_ERROR = 0x83,
} emuwire_msg_type_t;

/* ---- bitmasks ---- */
#define EMUWIRE_CAPABILITIES_I2C ((uint16_t)(1u << 0))
#define EMUWIRE_CAPABILITIES_SPI ((uint16_t)(1u << 1))
#define EMUWIRE_CAPABILITIES_PWM ((uint16_t)(1u << 2))
#define EMUWIRE_CAPABILITIES_DIGITAL ((uint16_t)(1u << 3))
#define EMUWIRE_CAPABILITIES_TRACE ((uint16_t)(1u << 4))

#define EMUWIRE_DEVICE_FLAGS_WRAP ((uint8_t)(1u << 0))
#define EMUWIRE_DEVICE_FLAGS_NACK_ON_RO_WRITE ((uint8_t)(1u << 1))
#define EMUWIRE_DEVICE_FLAGS_REPORT_ACCESS ((uint8_t)(1u << 2))

#define EMUWIRE_REGISTER_FLAGS_WRITABLE ((uint8_t)(1u << 0))
#define EMUWIRE_REGISTER_FLAGS_USER_SETTABLE ((uint8_t)(1u << 1))

#define EMUWIRE_TRACE_FLAGS_STOP_ON_FULL ((uint8_t)(1u << 0))

#define EMUWIRE_PIN_STATE_CLK ((uint8_t)(1u << 0))
#define EMUWIRE_PIN_STATE_DAT0 ((uint8_t)(1u << 1))
#define EMUWIRE_PIN_STATE_DAT1 ((uint8_t)(1u << 2))
#define EMUWIRE_PIN_STATE_CS ((uint8_t)(1u << 3))

#define EMUWIRE_TRACE_EDGE_FLAGS_OVERFLOW_GAP ((uint8_t)(1u << 0))
#define EMUWIRE_TRACE_EDGE_FLAGS_WRAPPED ((uint8_t)(1u << 1))

/* Bit 7 marks an asynchronous event: no lookup needed to route a frame. */
#define EMUWIRE_MSG_IS_ASYNC(t) (((t) & 0x80u) != 0u)

/* ---- structs ---- */
/* One entry in a BUS_LIST response. */
typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t protocol;  /* bus_protocol */
    uint8_t clk_pin;
    uint8_t dat0_pin;
    uint8_t dat1_pin;
    uint8_t cs_pin;
    uint8_t device_count;
    uint8_t pio_block;
    uint8_t pio_sm;
    uint8_t _pad;
    uint32_t nominal_clock_hz;
    uint32_t measured_clock_hz;
} emuwire_bus_info_t;
_Static_assert(sizeof(emuwire_bus_info_t) == 18, "emuwire_bus_info_t must be 18 bytes — packing changed");
#define EMUWIRE_BUS_INFO_BUS_ID_OFFSET 0u
#define EMUWIRE_BUS_INFO_PROTOCOL_OFFSET 1u
#define EMUWIRE_BUS_INFO_CLK_PIN_OFFSET 2u
#define EMUWIRE_BUS_INFO_DAT0_PIN_OFFSET 3u
#define EMUWIRE_BUS_INFO_DAT1_PIN_OFFSET 4u
#define EMUWIRE_BUS_INFO_CS_PIN_OFFSET 5u
#define EMUWIRE_BUS_INFO_DEVICE_COUNT_OFFSET 6u
#define EMUWIRE_BUS_INFO_PIO_BLOCK_OFFSET 7u
#define EMUWIRE_BUS_INFO_PIO_SM_OFFSET 8u
#define EMUWIRE_BUS_INFO__PAD_OFFSET 9u
#define EMUWIRE_BUS_INFO_NOMINAL_CLOCK_HZ_OFFSET 10u
#define EMUWIRE_BUS_INFO_MEASURED_CLOCK_HZ_OFFSET 14u

/* One register in a device's map, sent at DEV_ATTACH. The list is sparse — each entry carries its own address — so a device using 0xD0-0xFC costs 45 entries, not 256. For a streaming device (reg_addr_width = STREAMING) the addr field is the position in the stream, not a register address. */
typedef struct __attribute__((packed)) {
    uint16_t addr;
    uint8_t value;
    uint8_t flags;  /* register_flags */
} emuwire_register_def_t;
_Static_assert(sizeof(emuwire_register_def_t) == 4, "emuwire_register_def_t must be 4 bytes — packing changed");
#define EMUWIRE_REGISTER_DEF_ADDR_OFFSET 0u
#define EMUWIRE_REGISTER_DEF_VALUE_OFFSET 2u
#define EMUWIRE_REGISTER_DEF_FLAGS_OFFSET 3u

/* One entry in a FAULT_LIST response. */
typedef struct __attribute__((packed)) {
    uint8_t fault_id;
    uint8_t fault_type;  /* fault_type */
    uint8_t trigger;  /* fault_trigger */
    uint8_t probability_pct;
    uint16_t trigger_reg;
    uint16_t target_reg;
    uint16_t remaining;
    uint8_t access_filter;  /* access_filter */
    uint8_t _pad;
    uint32_t trigger_n;
    uint32_t param_a;
    uint32_t param_b;
    uint32_t fired_count;
} emuwire_fault_info_t;
_Static_assert(sizeof(emuwire_fault_info_t) == 28, "emuwire_fault_info_t must be 28 bytes — packing changed");
#define EMUWIRE_FAULT_INFO_FAULT_ID_OFFSET 0u
#define EMUWIRE_FAULT_INFO_FAULT_TYPE_OFFSET 1u
#define EMUWIRE_FAULT_INFO_TRIGGER_OFFSET 2u
#define EMUWIRE_FAULT_INFO_PROBABILITY_PCT_OFFSET 3u
#define EMUWIRE_FAULT_INFO_TRIGGER_REG_OFFSET 4u
#define EMUWIRE_FAULT_INFO_TARGET_REG_OFFSET 6u
#define EMUWIRE_FAULT_INFO_REMAINING_OFFSET 8u
#define EMUWIRE_FAULT_INFO_ACCESS_FILTER_OFFSET 10u
#define EMUWIRE_FAULT_INFO__PAD_OFFSET 11u
#define EMUWIRE_FAULT_INFO_TRIGGER_N_OFFSET 12u
#define EMUWIRE_FAULT_INFO_PARAM_A_OFFSET 16u
#define EMUWIRE_FAULT_INFO_PARAM_B_OFFSET 20u
#define EMUWIRE_FAULT_INFO_FIRED_COUNT_OFFSET 24u

/* One raw pin-state sample, streamed inside EVT_TRACE_DATA. */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint8_t pin_state;  /* pin_state */
    uint8_t flags;  /* trace_edge_flags */
    uint16_t _pad;
} emuwire_trace_edge_t;
_Static_assert(sizeof(emuwire_trace_edge_t) == 8, "emuwire_trace_edge_t must be 8 bytes — packing changed");
#define EMUWIRE_TRACE_EDGE_TIMESTAMP_OFFSET 0u
#define EMUWIRE_TRACE_EDGE_PIN_STATE_OFFSET 4u
#define EMUWIRE_TRACE_EDGE_FLAGS_OFFSET 5u
#define EMUWIRE_TRACE_EDGE__PAD_OFFSET 6u

/* ---- message payloads ---- */
typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint32_t uptime_ms;
} emuwire_ping_response_t;
_Static_assert(sizeof(emuwire_ping_response_t) == 5, "emuwire_ping_response_t must be 5 bytes — packing changed");
#define EMUWIRE_PING_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_PING_RESPONSE_UPTIME_MS_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint16_t protocol_version;
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
    uint8_t fw_version_patch;
    uint8_t board_id;  /* board_id */
    uint8_t mcu_id;  /* mcu_id */
    uint8_t pio_blocks;
    uint8_t sm_total;
    uint8_t sm_free;
    uint8_t pio_instr_free_0;
    uint8_t pio_instr_free_1;
    uint8_t pio_instr_free_2;
    uint8_t max_buses;
    uint8_t max_devices_per_bus;
    uint8_t max_faults_per_device;
    uint16_t max_payload_bytes;
    uint16_t capabilities;  /* capabilities */
    uint32_t trace_buffer_bytes;
    uint32_t trace_tick_hz;
    uint64_t unique_id;
} emuwire_info_response_t;
_Static_assert(sizeof(emuwire_info_response_t) == 37, "emuwire_info_response_t must be 37 bytes — packing changed");
#define EMUWIRE_INFO_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_INFO_RESPONSE_PROTOCOL_VERSION_OFFSET 1u
#define EMUWIRE_INFO_RESPONSE_FW_VERSION_MAJOR_OFFSET 3u
#define EMUWIRE_INFO_RESPONSE_FW_VERSION_MINOR_OFFSET 4u
#define EMUWIRE_INFO_RESPONSE_FW_VERSION_PATCH_OFFSET 5u
#define EMUWIRE_INFO_RESPONSE_BOARD_ID_OFFSET 6u
#define EMUWIRE_INFO_RESPONSE_MCU_ID_OFFSET 7u
#define EMUWIRE_INFO_RESPONSE_PIO_BLOCKS_OFFSET 8u
#define EMUWIRE_INFO_RESPONSE_SM_TOTAL_OFFSET 9u
#define EMUWIRE_INFO_RESPONSE_SM_FREE_OFFSET 10u
#define EMUWIRE_INFO_RESPONSE_PIO_INSTR_FREE_0_OFFSET 11u
#define EMUWIRE_INFO_RESPONSE_PIO_INSTR_FREE_1_OFFSET 12u
#define EMUWIRE_INFO_RESPONSE_PIO_INSTR_FREE_2_OFFSET 13u
#define EMUWIRE_INFO_RESPONSE_MAX_BUSES_OFFSET 14u
#define EMUWIRE_INFO_RESPONSE_MAX_DEVICES_PER_BUS_OFFSET 15u
#define EMUWIRE_INFO_RESPONSE_MAX_FAULTS_PER_DEVICE_OFFSET 16u
#define EMUWIRE_INFO_RESPONSE_MAX_PAYLOAD_BYTES_OFFSET 17u
#define EMUWIRE_INFO_RESPONSE_CAPABILITIES_OFFSET 19u
#define EMUWIRE_INFO_RESPONSE_TRACE_BUFFER_BYTES_OFFSET 21u
#define EMUWIRE_INFO_RESPONSE_TRACE_TICK_HZ_OFFSET 25u
#define EMUWIRE_INFO_RESPONSE_UNIQUE_ID_OFFSET 29u

typedef struct __attribute__((packed)) {
    uint8_t mode;  /* reset_mode */
} emuwire_reset_request_t;
_Static_assert(sizeof(emuwire_reset_request_t) == 1, "emuwire_reset_request_t must be 1 bytes — packing changed");
#define EMUWIRE_RESET_REQUEST_MODE_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_reset_response_t;
_Static_assert(sizeof(emuwire_reset_response_t) == 1, "emuwire_reset_response_t must be 1 bytes — packing changed");
#define EMUWIRE_RESET_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t protocol;  /* bus_protocol */
    uint8_t clk_pin;
    uint8_t dat0_pin;
    uint8_t dat1_pin;
    uint8_t cs_pin;
    uint8_t _pad;
    uint32_t nominal_clock_hz;
} emuwire_bus_create_request_t;
_Static_assert(sizeof(emuwire_bus_create_request_t) == 10, "emuwire_bus_create_request_t must be 10 bytes — packing changed");
#define EMUWIRE_BUS_CREATE_REQUEST_PROTOCOL_OFFSET 0u
#define EMUWIRE_BUS_CREATE_REQUEST_CLK_PIN_OFFSET 1u
#define EMUWIRE_BUS_CREATE_REQUEST_DAT0_PIN_OFFSET 2u
#define EMUWIRE_BUS_CREATE_REQUEST_DAT1_PIN_OFFSET 3u
#define EMUWIRE_BUS_CREATE_REQUEST_CS_PIN_OFFSET 4u
#define EMUWIRE_BUS_CREATE_REQUEST__PAD_OFFSET 5u
#define EMUWIRE_BUS_CREATE_REQUEST_NOMINAL_CLOCK_HZ_OFFSET 6u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t bus_id;
} emuwire_bus_create_response_t;
_Static_assert(sizeof(emuwire_bus_create_response_t) == 2, "emuwire_bus_create_response_t must be 2 bytes — packing changed");
#define EMUWIRE_BUS_CREATE_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_BUS_CREATE_RESPONSE_BUS_ID_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
} emuwire_bus_destroy_request_t;
_Static_assert(sizeof(emuwire_bus_destroy_request_t) == 1, "emuwire_bus_destroy_request_t must be 1 bytes — packing changed");
#define EMUWIRE_BUS_DESTROY_REQUEST_BUS_ID_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_bus_destroy_response_t;
_Static_assert(sizeof(emuwire_bus_destroy_response_t) == 1, "emuwire_bus_destroy_response_t must be 1 bytes — packing changed");
#define EMUWIRE_BUS_DESTROY_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t count;
    /* count elements follow */
    emuwire_bus_info_t buses[];
} emuwire_bus_list_response_t;
_Static_assert(sizeof(emuwire_bus_list_response_t) == 2, "emuwire_bus_list_response_t must be 2 bytes — packing changed");
#define EMUWIRE_BUS_LIST_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_BUS_LIST_RESPONSE_COUNT_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t address_mode;  /* address_mode */
    uint16_t address;
    uint8_t reg_addr_width;  /* reg_addr_width */
    uint8_t auto_increment;  /* auto_increment */
    uint8_t flags;  /* device_flags */
    uint8_t _pad;
    uint32_t clock_max_hz;
    char name[16];
    uint16_t reg_count;
    /* reg_count elements follow */
    emuwire_register_def_t registers[];
} emuwire_dev_attach_request_t;
_Static_assert(sizeof(emuwire_dev_attach_request_t) == 30, "emuwire_dev_attach_request_t must be 30 bytes — packing changed");
#define EMUWIRE_DEV_ATTACH_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_DEV_ATTACH_REQUEST_ADDRESS_MODE_OFFSET 1u
#define EMUWIRE_DEV_ATTACH_REQUEST_ADDRESS_OFFSET 2u
#define EMUWIRE_DEV_ATTACH_REQUEST_REG_ADDR_WIDTH_OFFSET 4u
#define EMUWIRE_DEV_ATTACH_REQUEST_AUTO_INCREMENT_OFFSET 5u
#define EMUWIRE_DEV_ATTACH_REQUEST_FLAGS_OFFSET 6u
#define EMUWIRE_DEV_ATTACH_REQUEST__PAD_OFFSET 7u
#define EMUWIRE_DEV_ATTACH_REQUEST_CLOCK_MAX_HZ_OFFSET 8u
#define EMUWIRE_DEV_ATTACH_REQUEST_NAME_OFFSET 12u
#define EMUWIRE_DEV_ATTACH_REQUEST_REG_COUNT_OFFSET 28u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t dev_id;
} emuwire_dev_attach_response_t;
_Static_assert(sizeof(emuwire_dev_attach_response_t) == 2, "emuwire_dev_attach_response_t must be 2 bytes — packing changed");
#define EMUWIRE_DEV_ATTACH_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_DEV_ATTACH_RESPONSE_DEV_ID_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
} emuwire_dev_detach_request_t;
_Static_assert(sizeof(emuwire_dev_detach_request_t) == 2, "emuwire_dev_detach_request_t must be 2 bytes — packing changed");
#define EMUWIRE_DEV_DETACH_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_DEV_DETACH_REQUEST_DEV_ID_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_dev_detach_response_t;
_Static_assert(sizeof(emuwire_dev_detach_response_t) == 1, "emuwire_dev_detach_response_t must be 1 bytes — packing changed");
#define EMUWIRE_DEV_DETACH_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint16_t start_reg;
    uint16_t count;
    /* count elements follow */
    uint8_t data[];
} emuwire_dev_write_regs_request_t;
_Static_assert(sizeof(emuwire_dev_write_regs_request_t) == 6, "emuwire_dev_write_regs_request_t must be 6 bytes — packing changed");
#define EMUWIRE_DEV_WRITE_REGS_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_DEV_WRITE_REGS_REQUEST_DEV_ID_OFFSET 1u
#define EMUWIRE_DEV_WRITE_REGS_REQUEST_START_REG_OFFSET 2u
#define EMUWIRE_DEV_WRITE_REGS_REQUEST_COUNT_OFFSET 4u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_dev_write_regs_response_t;
_Static_assert(sizeof(emuwire_dev_write_regs_response_t) == 1, "emuwire_dev_write_regs_response_t must be 1 bytes — packing changed");
#define EMUWIRE_DEV_WRITE_REGS_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint16_t start_reg;
    uint16_t count;
} emuwire_dev_read_regs_request_t;
_Static_assert(sizeof(emuwire_dev_read_regs_request_t) == 6, "emuwire_dev_read_regs_request_t must be 6 bytes — packing changed");
#define EMUWIRE_DEV_READ_REGS_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_DEV_READ_REGS_REQUEST_DEV_ID_OFFSET 1u
#define EMUWIRE_DEV_READ_REGS_REQUEST_START_REG_OFFSET 2u
#define EMUWIRE_DEV_READ_REGS_REQUEST_COUNT_OFFSET 4u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint16_t count;
    /* count elements follow */
    uint8_t data[];
} emuwire_dev_read_regs_response_t;
_Static_assert(sizeof(emuwire_dev_read_regs_response_t) == 3, "emuwire_dev_read_regs_response_t must be 3 bytes — packing changed");
#define EMUWIRE_DEV_READ_REGS_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_DEV_READ_REGS_RESPONSE_COUNT_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint8_t fault_type;  /* fault_type */
    uint8_t trigger;  /* fault_trigger */
    uint16_t trigger_reg;
    uint16_t target_reg;
    uint16_t repeat_count;
    uint8_t probability_pct;
    uint8_t access_filter;  /* access_filter */
    uint32_t trigger_n;
    uint32_t param_a;
    uint32_t param_b;
} emuwire_fault_set_request_t;
_Static_assert(sizeof(emuwire_fault_set_request_t) == 24, "emuwire_fault_set_request_t must be 24 bytes — packing changed");
#define EMUWIRE_FAULT_SET_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_FAULT_SET_REQUEST_DEV_ID_OFFSET 1u
#define EMUWIRE_FAULT_SET_REQUEST_FAULT_TYPE_OFFSET 2u
#define EMUWIRE_FAULT_SET_REQUEST_TRIGGER_OFFSET 3u
#define EMUWIRE_FAULT_SET_REQUEST_TRIGGER_REG_OFFSET 4u
#define EMUWIRE_FAULT_SET_REQUEST_TARGET_REG_OFFSET 6u
#define EMUWIRE_FAULT_SET_REQUEST_REPEAT_COUNT_OFFSET 8u
#define EMUWIRE_FAULT_SET_REQUEST_PROBABILITY_PCT_OFFSET 10u
#define EMUWIRE_FAULT_SET_REQUEST_ACCESS_FILTER_OFFSET 11u
#define EMUWIRE_FAULT_SET_REQUEST_TRIGGER_N_OFFSET 12u
#define EMUWIRE_FAULT_SET_REQUEST_PARAM_A_OFFSET 16u
#define EMUWIRE_FAULT_SET_REQUEST_PARAM_B_OFFSET 20u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t fault_id;
} emuwire_fault_set_response_t;
_Static_assert(sizeof(emuwire_fault_set_response_t) == 2, "emuwire_fault_set_response_t must be 2 bytes — packing changed");
#define EMUWIRE_FAULT_SET_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_FAULT_SET_RESPONSE_FAULT_ID_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint8_t fault_id;
} emuwire_fault_clear_request_t;
_Static_assert(sizeof(emuwire_fault_clear_request_t) == 3, "emuwire_fault_clear_request_t must be 3 bytes — packing changed");
#define EMUWIRE_FAULT_CLEAR_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_FAULT_CLEAR_REQUEST_DEV_ID_OFFSET 1u
#define EMUWIRE_FAULT_CLEAR_REQUEST_FAULT_ID_OFFSET 2u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_fault_clear_response_t;
_Static_assert(sizeof(emuwire_fault_clear_response_t) == 1, "emuwire_fault_clear_response_t must be 1 bytes — packing changed");
#define EMUWIRE_FAULT_CLEAR_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
} emuwire_fault_list_request_t;
_Static_assert(sizeof(emuwire_fault_list_request_t) == 2, "emuwire_fault_list_request_t must be 2 bytes — packing changed");
#define EMUWIRE_FAULT_LIST_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_FAULT_LIST_REQUEST_DEV_ID_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t count;
    /* count elements follow */
    emuwire_fault_info_t faults[];
} emuwire_fault_list_response_t;
_Static_assert(sizeof(emuwire_fault_list_response_t) == 2, "emuwire_fault_list_response_t must be 2 bytes — packing changed");
#define EMUWIRE_FAULT_LIST_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_FAULT_LIST_RESPONSE_COUNT_OFFSET 1u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t flags;  /* trace_flags */
    uint16_t _pad;
    uint32_t max_bytes;
} emuwire_trace_start_request_t;
_Static_assert(sizeof(emuwire_trace_start_request_t) == 8, "emuwire_trace_start_request_t must be 8 bytes — packing changed");
#define EMUWIRE_TRACE_START_REQUEST_BUS_ID_OFFSET 0u
#define EMUWIRE_TRACE_START_REQUEST_FLAGS_OFFSET 1u
#define EMUWIRE_TRACE_START_REQUEST__PAD_OFFSET 2u
#define EMUWIRE_TRACE_START_REQUEST_MAX_BYTES_OFFSET 4u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
} emuwire_trace_start_response_t;
_Static_assert(sizeof(emuwire_trace_start_response_t) == 1, "emuwire_trace_start_response_t must be 1 bytes — packing changed");
#define EMUWIRE_TRACE_START_RESPONSE_STATUS_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
} emuwire_trace_stop_request_t;
_Static_assert(sizeof(emuwire_trace_stop_request_t) == 1, "emuwire_trace_stop_request_t must be 1 bytes — packing changed");
#define EMUWIRE_TRACE_STOP_REQUEST_BUS_ID_OFFSET 0u

typedef struct __attribute__((packed)) {
    uint8_t status;  /* status */
    uint8_t overflowed;
    uint16_t _pad;
    uint32_t captured_edges;
    uint32_t dropped_edges;
} emuwire_trace_stop_response_t;
_Static_assert(sizeof(emuwire_trace_stop_response_t) == 12, "emuwire_trace_stop_response_t must be 12 bytes — packing changed");
#define EMUWIRE_TRACE_STOP_RESPONSE_STATUS_OFFSET 0u
#define EMUWIRE_TRACE_STOP_RESPONSE_OVERFLOWED_OFFSET 1u
#define EMUWIRE_TRACE_STOP_RESPONSE__PAD_OFFSET 2u
#define EMUWIRE_TRACE_STOP_RESPONSE_CAPTURED_EDGES_OFFSET 4u
#define EMUWIRE_TRACE_STOP_RESPONSE_DROPPED_EDGES_OFFSET 8u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint8_t access;  /* access_kind */
    uint8_t value;
    uint16_t reg;
    uint16_t _pad;
    uint64_t timestamp_us;
} emuwire_evt_dev_access_payload_t;
_Static_assert(sizeof(emuwire_evt_dev_access_payload_t) == 16, "emuwire_evt_dev_access_payload_t must be 16 bytes — packing changed");
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_BUS_ID_OFFSET 0u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_DEV_ID_OFFSET 1u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_ACCESS_OFFSET 2u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_VALUE_OFFSET 3u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_REG_OFFSET 4u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD__PAD_OFFSET 6u
#define EMUWIRE_EVT_DEV_ACCESS_PAYLOAD_TIMESTAMP_US_OFFSET 8u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t dev_id;
    uint8_t fault_id;
    uint8_t fault_type;  /* fault_type */
    uint16_t remaining;
    uint16_t _pad;
    uint64_t timestamp_us;
} emuwire_evt_fault_fired_payload_t;
_Static_assert(sizeof(emuwire_evt_fault_fired_payload_t) == 16, "emuwire_evt_fault_fired_payload_t must be 16 bytes — packing changed");
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_BUS_ID_OFFSET 0u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_DEV_ID_OFFSET 1u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_FAULT_ID_OFFSET 2u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_FAULT_TYPE_OFFSET 3u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_REMAINING_OFFSET 4u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD__PAD_OFFSET 6u
#define EMUWIRE_EVT_FAULT_FIRED_PAYLOAD_TIMESTAMP_US_OFFSET 8u

typedef struct __attribute__((packed)) {
    uint8_t bus_id;
    uint8_t overflowed;
    uint16_t edge_count;
    uint32_t chunk_index;
    /* edge_count elements follow */
    emuwire_trace_edge_t edges[];
} emuwire_evt_trace_data_payload_t;
_Static_assert(sizeof(emuwire_evt_trace_data_payload_t) == 8, "emuwire_evt_trace_data_payload_t must be 8 bytes — packing changed");
#define EMUWIRE_EVT_TRACE_DATA_PAYLOAD_BUS_ID_OFFSET 0u
#define EMUWIRE_EVT_TRACE_DATA_PAYLOAD_OVERFLOWED_OFFSET 1u
#define EMUWIRE_EVT_TRACE_DATA_PAYLOAD_EDGE_COUNT_OFFSET 2u
#define EMUWIRE_EVT_TRACE_DATA_PAYLOAD_CHUNK_INDEX_OFFSET 4u

typedef struct __attribute__((packed)) {
    uint8_t code;  /* status */
    uint8_t context_type;  /* msg_type */
    uint8_t context_seq;
    uint8_t bus_id;
    uint8_t dev_id;
    uint8_t _pad;
    uint16_t _pad2;
    uint32_t detail;
    uint64_t timestamp_us;
} emuwire_evt_error_payload_t;
_Static_assert(sizeof(emuwire_evt_error_payload_t) == 20, "emuwire_evt_error_payload_t must be 20 bytes — packing changed");
#define EMUWIRE_EVT_ERROR_PAYLOAD_CODE_OFFSET 0u
#define EMUWIRE_EVT_ERROR_PAYLOAD_CONTEXT_TYPE_OFFSET 1u
#define EMUWIRE_EVT_ERROR_PAYLOAD_CONTEXT_SEQ_OFFSET 2u
#define EMUWIRE_EVT_ERROR_PAYLOAD_BUS_ID_OFFSET 3u
#define EMUWIRE_EVT_ERROR_PAYLOAD_DEV_ID_OFFSET 4u
#define EMUWIRE_EVT_ERROR_PAYLOAD__PAD_OFFSET 5u
#define EMUWIRE_EVT_ERROR_PAYLOAD__PAD2_OFFSET 6u
#define EMUWIRE_EVT_ERROR_PAYLOAD_DETAIL_OFFSET 8u
#define EMUWIRE_EVT_ERROR_PAYLOAD_TIMESTAMP_US_OFFSET 12u

#endif /* EMUWIRE_MESSAGES_H */
