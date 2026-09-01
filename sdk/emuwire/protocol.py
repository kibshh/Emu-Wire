"""DO NOT EDIT — generated from protocol/protocol.yaml by protocol/codegen.py.

Hand-editing this file makes the host SDK and the firmware disagree about the
wire format, and CI fails on the difference. Change the spec under protocol/
and re-run:  python protocol/codegen.py
"""


from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, ClassVar, Dict, List, Optional, Tuple

PROTOCOL_VERSION = 1

FRAME_MAGIC = 0xa5
FRAME_HEADER_BYTES = 5
FRAME_TRAILER_BYTES = 2
FRAME_OVERHEAD_BYTES = 7
MAX_PAYLOAD_BYTES = 8192
SEQ_ASYNC = 0
SEQ_MIN, SEQ_MAX = 1, 255

# Named sentinels. Several share a value but mean different things.
PIN_UNUSED = 0xff
DEV_ID_ALL = 0xff
FAULT_ID_ALL = 0xff
DEV_ID_NONE = 0xff
BUS_ID_NONE = 0xff
FAULT_REPEAT_UNLIMITED = 0xffff

# CRC-16/IBM-3740: poly=0x1021 init=0xffff refin=False refout=False xorout=0x0000
CRC_POLY = 0x1021
CRC_INIT = 0xffff
CRC_XOROUT = 0x0000
CRC_CHECK = 0x29b1  # crc16(b'123456789')

def _build_table() -> List[int]:
    table = []
    for byte in range(256):
        reg = byte << 8
        for _ in range(8):
            reg = ((reg << 1) ^ CRC_POLY) & 0xFFFF if reg & 0x8000 else (reg << 1) & 0xFFFF
        table.append(reg)
    return table

_CRC_TABLE = _build_table()

def crc16(data: bytes) -> int:
    """CRC over TYPE..PAYLOAD. Must agree byte for byte with crc16.c."""
    reg = CRC_INIT
    for byte in data:
        reg = ((reg << 8) & 0xFFFF) ^ _CRC_TABLE[((reg >> 8) ^ byte) & 0xFF]
    return reg ^ CRC_XOROUT

def encode_frame(msg_type: int, seq: int, payload: bytes = b'') -> bytes:
    """Wrap a payload in a frame. The CRC covers TYPE..PAYLOAD, not MAGIC."""
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ValueError(
            f'payload is {len(payload)} bytes, over the {MAX_PAYLOAD_BYTES} byte limit'
        )
    body = struct.pack('<BBH', msg_type, seq, len(payload)) + payload
    return bytes([FRAME_MAGIC]) + body + struct.pack('<H', crc16(body))

def decode_frame(buf: bytes) -> Tuple[Optional[Tuple[int, int, bytes]], int]:
    """Find the first valid frame in buf.

    Returns ((msg_type, seq, payload), bytes_consumed), or (None, bytes_consumed)
    when no complete frame is present yet. Never raises on malformed input and
    never reads past the end of buf: garbage must resync, not wedge.
    """
    i = 0
    while True:
        start = buf.find(bytes([FRAME_MAGIC]), i)
        if start < 0:
            return None, len(buf)          # no magic at all, discard everything
        if len(buf) - start < FRAME_OVERHEAD_BYTES:
            return None, start             # header incomplete, keep the remainder
        msg_type, seq, length = struct.unpack_from('<BBH', buf, start + 1)
        if length > MAX_PAYLOAD_BYTES:
            i = start + 1                  # implausible length: this magic was noise
            continue
        end = start + FRAME_OVERHEAD_BYTES + length
        if len(buf) < end:
            return None, start             # payload incomplete, wait for more
        body = buf[start + 1:end - FRAME_TRAILER_BYTES]
        got = struct.unpack_from('<H', buf, end - FRAME_TRAILER_BYTES)[0]
        if got != crc16(body):
            i = start + 1                  # bad CRC: rescan from the next byte,
            continue                       # never trust the length that failed
        return (msg_type, seq, bytes(buf[start + FRAME_HEADER_BYTES:end - FRAME_TRAILER_BYTES])), end

class Status(IntEnum):
    OK = 0x00
    ERR_BAD_LENGTH = 0x01
    ERR_BAD_CRC = 0x02
    ERR_UNKNOWN_TYPE = 0x03
    ERR_BAD_PARAM = 0x04
    ERR_NO_SUCH_BUS = 0x05
    ERR_NO_SUCH_DEVICE = 0x06
    ERR_ADDRESS_IN_USE = 0x07
    ERR_ADDRESS_RESERVED = 0x08
    ERR_BUS_LIMIT = 0x09
    ERR_DEVICE_LIMIT = 0x0a
    ERR_NO_FREE_SM = 0x0b
    ERR_PIO_PROGRAM_SPACE = 0x0c
    ERR_PIN_UNAVAILABLE = 0x0d
    ERR_PIN_IN_USE = 0x0e
    ERR_UNSUPPORTED_PROTOCOL = 0x0f
    ERR_REG_OUT_OF_RANGE = 0x10
    ERR_REG_READ_ONLY = 0x11
    ERR_NO_SUCH_FAULT = 0x12
    ERR_FAULT_LIMIT = 0x13
    ERR_UNSUPPORTED_FAULT = 0x14
    ERR_TRACE_ACTIVE = 0x15
    ERR_TRACE_NOT_ACTIVE = 0x16
    ERR_TRACE_OVERFLOW = 0x17
    ERR_CLOCK_OUT_OF_SPEC = 0x18
    ERR_BUSY = 0x19
    ERR_NOT_IMPLEMENTED = 0x1a
    ERR_INTERNAL = 0x1b

class AddressMode(IntEnum):
    ADDR_7BIT = 0x00
    ADDR_10BIT = 0x01
    ADDR_CHIP_SELECT = 0x02

class RegAddrWidth(IntEnum):
    STREAMING = 0x00
    WIDTH_8 = 0x01
    WIDTH_16 = 0x02

class AutoIncrement(IntEnum):
    NONE = 0x00
    ON_READ = 0x01
    ON_WRITE = 0x02
    BOTH = 0x03

class AccessKind(IntEnum):
    READ = 0x00
    WRITE = 0x01

class FaultType(IntEnum):
    NACK_ON_READ = 0x01
    NACK_ON_WRITE = 0x02
    BUS_HANG = 0x03
    CORRUPT_REGISTER = 0x04
    OUT_OF_RANGE = 0x05
    STUCK_VALUE = 0x06
    CLOCK_STRETCH = 0x07
    SETUP_VIOLATION = 0x40
    HOLD_VIOLATION = 0x41
    PARTIAL_BYTE = 0x42
    ACK_GLITCH = 0x43
    CLOCK_SKEW = 0x44

class FaultTrigger(IntEnum):
    IMMEDIATE = 0x00
    AFTER_N_ACCESS = 0x01
    ON_NTH_READ_REG = 0x02
    AFTER_MS = 0x03
    RANDOM = 0x04

class BoardId(IntEnum):
    UNKNOWN = 0x00
    PICO2 = 0x01
    PICO2W = 0x02
    CUSTOM = 0xff

class McuId(IntEnum):
    RP2350A = 0x01
    RP2350B = 0x02

class ResetMode(IntEnum):
    SOFT = 0x00
    BOOTSEL = 0x01

class BusProtocol(IntEnum):
    I2C = 0x01
    SPI = 0x02

# Message templates, so no bare status code ever reaches a user.
STATUS_MESSAGES: Dict[int, str] = {
    Status.OK: "Success.",
    Status.ERR_BAD_LENGTH: "Frame length {len} exceeds the {max} byte limit.",
    Status.ERR_BAD_CRC: "Frame CRC mismatch — expected {expected:#06x}, got {actual:#06x}.",
    Status.ERR_UNKNOWN_TYPE: "Unknown message type {type:#04x}. Firmware protocol version is {fw_version}, the SDK expects {sdk_version}.",
    Status.ERR_BAD_PARAM: "Parameter '{param}' is invalid: {reason}.",
    Status.ERR_NO_SUCH_BUS: "No bus with id {bus_id}. Create one with i2c_bus() first.",
    Status.ERR_NO_SUCH_DEVICE: "No device with id {dev_id} on bus {bus_id}.",
    Status.ERR_ADDRESS_IN_USE: "Address {address:#04x} is already taken by '{existing}' on this bus. Two devices cannot share an address — that is physically impossible on real hardware.",
    Status.ERR_ADDRESS_RESERVED: "Address {address:#04x} is reserved by the I2C specification. Reserved ranges are 0x00-0x07 and 0x78-0x7F.",
    Status.ERR_BUS_LIMIT: "All {max} buses are in use.",
    Status.ERR_DEVICE_LIMIT: "Bus {bus_id} already has the maximum of {max} devices.",
    Status.ERR_NO_FREE_SM: "No free PIO state machine. {in_use} of {total} are in use: {detail}.",
    Status.ERR_PIO_PROGRAM_SPACE: "PIO block {block} has {free} of 32 instructions free; the {program} program needs {needed}.",
    Status.ERR_PIN_UNAVAILABLE: "Pin GP{pin} cannot be used for {function} on {board}. Valid pins are {valid}.",
    Status.ERR_PIN_IN_USE: "Pin GP{pin} is already used by bus {bus_id} as {function}.",
    Status.ERR_UNSUPPORTED_PROTOCOL: "Protocol '{protocol}' is not supported by this firmware. Supported: {supported}.",
    Status.ERR_REG_OUT_OF_RANGE: "Register {reg:#04x} is outside the map for '{device}', which covers {first:#04x}-{last:#04x}.",
    Status.ERR_REG_READ_ONLY: "Register {reg:#04x} on '{device}' is read-only.",
    Status.ERR_NO_SUCH_FAULT: "No fault with id {fault_id} on device {dev_id}.",
    Status.ERR_FAULT_LIMIT: "Device {dev_id} already has the maximum of {max} active faults.",
    Status.ERR_UNSUPPORTED_FAULT: "Fault '{fault}' is not supported on a {protocol} bus.",
    Status.ERR_TRACE_ACTIVE: "A trace is already running on bus {bus_id}. Stop it before starting another.",
    Status.ERR_TRACE_NOT_ACTIVE: "No trace is running on bus {bus_id}.",
    Status.ERR_TRACE_OVERFLOW: "Trace buffer overflowed; {dropped} edges were lost. The capture has a gap and must not be trusted.",
    Status.ERR_CLOCK_OUT_OF_SPEC: "The DUT is clocking bus {bus_id} at {actual_hz} Hz, above the {max_hz} Hz rating of '{device}'. The device is misbehaving as real silicon would.",
    Status.ERR_BUSY: "Device is busy servicing the bus; retry.",
    Status.ERR_NOT_IMPLEMENTED: "'{feature}' is accepted by the protocol but not implemented in firmware {fw_version}.",
    Status.ERR_INTERNAL: "Internal firmware error at {file}:{line}. This is a bug — please report it.",
}

class Capabilities(IntEnum):
    I2C = 1 << 0
    SPI = 1 << 1
    PWM = 1 << 2
    DIGITAL = 1 << 3
    TRACE = 1 << 4

class DeviceFlags(IntEnum):
    WRAP = 1 << 0
    NACK_ON_RO_WRITE = 1 << 1
    REPORT_ACCESS = 1 << 2

class RegisterFlags(IntEnum):
    WRITABLE = 1 << 0
    USER_SETTABLE = 1 << 1

class TraceFlags(IntEnum):
    STOP_ON_FULL = 1 << 0

class TraceEdgeFlags(IntEnum):
    OVERFLOW_GAP = 1 << 0
    WRAPPED = 1 << 1

# Per-bus semantics, so the SDK can reject a request locally with a
# specific message instead of sending one the board will refuse.
BUS_PINS: Dict[int, Dict[str, Optional[str]]] = {
    BusProtocol.I2C: {"clk_pin": 'SCL', "dat0_pin": 'SDA', "dat1_pin": None, "cs_pin": None},
    BusProtocol.SPI: {"clk_pin": 'SCK', "dat0_pin": 'MOSI', "dat1_pin": 'MISO', "cs_pin": 'CS'},
}
BUS_SUPPORTED_FAULTS: Dict[int, Tuple[int, ...]] = {
    BusProtocol.I2C: (FaultType.NACK_ON_READ, FaultType.NACK_ON_WRITE, FaultType.BUS_HANG, FaultType.CORRUPT_REGISTER, FaultType.OUT_OF_RANGE, FaultType.STUCK_VALUE, FaultType.CLOCK_STRETCH, FaultType.SETUP_VIOLATION, FaultType.HOLD_VIOLATION, FaultType.PARTIAL_BYTE, FaultType.ACK_GLITCH, FaultType.CLOCK_SKEW,),
    BusProtocol.SPI: (FaultType.CORRUPT_REGISTER, FaultType.OUT_OF_RANGE, FaultType.STUCK_VALUE, FaultType.SETUP_VIOLATION, FaultType.HOLD_VIOLATION, FaultType.PARTIAL_BYTE,),
}
BUS_ADDRESS_MODES: Dict[int, Tuple[int, ...]] = {
    BusProtocol.I2C: (AddressMode.ADDR_7BIT, AddressMode.ADDR_10BIT,),
    BusProtocol.SPI: (AddressMode.ADDR_CHIP_SELECT,),
}
# Reserved by the I2C specification; rejected before anything reaches the board.
I2C_RESERVED_ADDRESSES: Tuple[Tuple[int, int], ...] = (
    (0x00, 0x07),  # General call, START byte, CBUS, and reserved for future use
    (0x78, 0x7f),  # 10-bit addressing prefix and reserved for future use
)

@dataclass
class BusInfo:
    bus_id: int = 0
    protocol: int = 0
    clk_pin: int = 0
    dat0_pin: int = 0
    dat1_pin: int = 0
    cs_pin: int = 0
    device_count: int = 0
    pio_block: int = 0
    pio_sm: int = 0
    _pad: int = 0
    nominal_clock_hz: int = 0
    measured_clock_hz: int = 0
    FORMAT: ClassVar[str] = "<BBBBBBBBBBII"
    FIXED_SIZE: ClassVar[int] = 18

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.protocol, self.clk_pin, self.dat0_pin, self.dat1_pin, self.cs_pin, self.device_count, self.pio_block, self.pio_sm, self._pad, self.nominal_clock_hz, self.measured_clock_hz)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusInfo":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusInfo needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, protocol, clk_pin, dat0_pin, dat1_pin, cs_pin, device_count, pio_block, pio_sm, _pad, nominal_clock_hz, measured_clock_hz = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, protocol=protocol, clk_pin=clk_pin, dat0_pin=dat0_pin, dat1_pin=dat1_pin, cs_pin=cs_pin, device_count=device_count, pio_block=pio_block, pio_sm=pio_sm, _pad=_pad, nominal_clock_hz=nominal_clock_hz, measured_clock_hz=measured_clock_hz)

@dataclass
class RegisterDef:
    addr: int = 0
    value: int = 0
    flags: int = 0
    FORMAT: ClassVar[str] = "<HBB"
    FIXED_SIZE: ClassVar[int] = 4

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.addr, self.value, self.flags)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "RegisterDef":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'RegisterDef needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        addr, value, flags = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(addr=addr, value=value, flags=flags)

@dataclass
class FaultInfo:
    fault_id: int = 0
    fault_type: int = 0
    trigger: int = 0
    probability_pct: int = 0
    trigger_reg: int = 0
    remaining: int = 0
    trigger_n: int = 0
    param_a: int = 0
    param_b: int = 0
    fired_count: int = 0
    FORMAT: ClassVar[str] = "<BBBBHHIIII"
    FIXED_SIZE: ClassVar[int] = 24

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.fault_id, self.fault_type, self.trigger, self.probability_pct, self.trigger_reg, self.remaining, self.trigger_n, self.param_a, self.param_b, self.fired_count)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultInfo":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultInfo needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        fault_id, fault_type, trigger, probability_pct, trigger_reg, remaining, trigger_n, param_a, param_b, fired_count = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(fault_id=fault_id, fault_type=fault_type, trigger=trigger, probability_pct=probability_pct, trigger_reg=trigger_reg, remaining=remaining, trigger_n=trigger_n, param_a=param_a, param_b=param_b, fired_count=fired_count)

@dataclass
class TraceEdge:
    timestamp: int = 0
    pin_state: int = 0
    flags: int = 0
    _pad: int = 0
    FORMAT: ClassVar[str] = "<IBBH"
    FIXED_SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.timestamp, self.pin_state, self.flags, self._pad)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "TraceEdge":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'TraceEdge needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        timestamp, pin_state, flags, _pad = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(timestamp=timestamp, pin_state=pin_state, flags=flags, _pad=_pad)

@dataclass
class PingRequest:
    pass
    TYPE: ClassVar[int] = 0x01
    FORMAT: ClassVar[str] = "<"
    FIXED_SIZE: ClassVar[int] = 0

    def pack(self) -> bytes:
        out = b''
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "PingRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'PingRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        return cls()

@dataclass
class PingResponse:
    status: int = 0
    uptime_ms: int = 0
    TYPE: ClassVar[int] = 0x01
    FORMAT: ClassVar[str] = "<BI"
    FIXED_SIZE: ClassVar[int] = 5

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.uptime_ms)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "PingResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'PingResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, uptime_ms = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, uptime_ms=uptime_ms)

@dataclass
class InfoRequest:
    pass
    TYPE: ClassVar[int] = 0x02
    FORMAT: ClassVar[str] = "<"
    FIXED_SIZE: ClassVar[int] = 0

    def pack(self) -> bytes:
        out = b''
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "InfoRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'InfoRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        return cls()

@dataclass
class InfoResponse:
    status: int = 0
    protocol_version: int = 0
    fw_version_major: int = 0
    fw_version_minor: int = 0
    fw_version_patch: int = 0
    board_id: int = 0
    mcu_id: int = 0
    pio_blocks: int = 0
    sm_total: int = 0
    sm_free: int = 0
    pio_instr_free_0: int = 0
    pio_instr_free_1: int = 0
    pio_instr_free_2: int = 0
    max_buses: int = 0
    max_devices_per_bus: int = 0
    max_faults_per_device: int = 0
    max_payload_bytes: int = 0
    capabilities: int = 0
    trace_buffer_bytes: int = 0
    trace_tick_hz: int = 0
    unique_id: int = 0
    TYPE: ClassVar[int] = 0x02
    FORMAT: ClassVar[str] = "<BHBBBBBBBBBBBBBBHHIIQ"
    FIXED_SIZE: ClassVar[int] = 37

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.protocol_version, self.fw_version_major, self.fw_version_minor, self.fw_version_patch, self.board_id, self.mcu_id, self.pio_blocks, self.sm_total, self.sm_free, self.pio_instr_free_0, self.pio_instr_free_1, self.pio_instr_free_2, self.max_buses, self.max_devices_per_bus, self.max_faults_per_device, self.max_payload_bytes, self.capabilities, self.trace_buffer_bytes, self.trace_tick_hz, self.unique_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "InfoResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'InfoResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, protocol_version, fw_version_major, fw_version_minor, fw_version_patch, board_id, mcu_id, pio_blocks, sm_total, sm_free, pio_instr_free_0, pio_instr_free_1, pio_instr_free_2, max_buses, max_devices_per_bus, max_faults_per_device, max_payload_bytes, capabilities, trace_buffer_bytes, trace_tick_hz, unique_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, protocol_version=protocol_version, fw_version_major=fw_version_major, fw_version_minor=fw_version_minor, fw_version_patch=fw_version_patch, board_id=board_id, mcu_id=mcu_id, pio_blocks=pio_blocks, sm_total=sm_total, sm_free=sm_free, pio_instr_free_0=pio_instr_free_0, pio_instr_free_1=pio_instr_free_1, pio_instr_free_2=pio_instr_free_2, max_buses=max_buses, max_devices_per_bus=max_devices_per_bus, max_faults_per_device=max_faults_per_device, max_payload_bytes=max_payload_bytes, capabilities=capabilities, trace_buffer_bytes=trace_buffer_bytes, trace_tick_hz=trace_tick_hz, unique_id=unique_id)

@dataclass
class ResetRequest:
    mode: int = 0
    TYPE: ClassVar[int] = 0x03
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.mode)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "ResetRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'ResetRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        mode, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(mode=mode)

@dataclass
class ResetResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x03
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "ResetResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'ResetResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class BusCreateRequest:
    protocol: int = 0
    clk_pin: int = 0
    dat0_pin: int = 0
    dat1_pin: int = 0
    cs_pin: int = 0
    _pad: int = 0
    nominal_clock_hz: int = 0
    TYPE: ClassVar[int] = 0x10
    FORMAT: ClassVar[str] = "<BBBBBBI"
    FIXED_SIZE: ClassVar[int] = 10

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.protocol, self.clk_pin, self.dat0_pin, self.dat1_pin, self.cs_pin, self._pad, self.nominal_clock_hz)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusCreateRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusCreateRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        protocol, clk_pin, dat0_pin, dat1_pin, cs_pin, _pad, nominal_clock_hz = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(protocol=protocol, clk_pin=clk_pin, dat0_pin=dat0_pin, dat1_pin=dat1_pin, cs_pin=cs_pin, _pad=_pad, nominal_clock_hz=nominal_clock_hz)

@dataclass
class BusCreateResponse:
    status: int = 0
    bus_id: int = 0
    TYPE: ClassVar[int] = 0x10
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.bus_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusCreateResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusCreateResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, bus_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, bus_id=bus_id)

@dataclass
class BusDestroyRequest:
    bus_id: int = 0
    TYPE: ClassVar[int] = 0x11
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusDestroyRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusDestroyRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id)

@dataclass
class BusDestroyResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x11
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusDestroyResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusDestroyResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class BusListRequest:
    pass
    TYPE: ClassVar[int] = 0x12
    FORMAT: ClassVar[str] = "<"
    FIXED_SIZE: ClassVar[int] = 0

    def pack(self) -> bytes:
        out = b''
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusListRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusListRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        return cls()

@dataclass
class BusListResponse:
    status: int = 0
    count: int = 0
    buses: List[BusInfo] = field(default_factory=list)
    TYPE: ClassVar[int] = 0x12
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.count)
        for item in self.buses:
            out += item.pack()
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "BusListResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'BusListResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, count = struct.unpack_from(cls.FORMAT, data, 0)
        items = []
        off = cls.FIXED_SIZE
        for _ in range(count):
            items.append(BusInfo.unpack(data[off:]))
            off += BusInfo.FIXED_SIZE
        return cls(status=status, count=count, buses=items)

@dataclass
class DevAttachRequest:
    bus_id: int = 0
    address_mode: int = 0
    address: int = 0
    reg_addr_width: int = 0
    auto_increment: int = 0
    flags: int = 0
    _pad: int = 0
    clock_max_hz: int = 0
    name: bytes = b''
    reg_count: int = 0
    registers: List[RegisterDef] = field(default_factory=list)
    TYPE: ClassVar[int] = 0x20
    FORMAT: ClassVar[str] = "<BBHBBBBI16sH"
    FIXED_SIZE: ClassVar[int] = 30

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.address_mode, self.address, self.reg_addr_width, self.auto_increment, self.flags, self._pad, self.clock_max_hz, self.name.ljust(16, b'\0'), self.reg_count)
        for item in self.registers:
            out += item.pack()
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevAttachRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevAttachRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, address_mode, address, reg_addr_width, auto_increment, flags, _pad, clock_max_hz, name, reg_count = struct.unpack_from(cls.FORMAT, data, 0)
        items = []
        off = cls.FIXED_SIZE
        for _ in range(reg_count):
            items.append(RegisterDef.unpack(data[off:]))
            off += RegisterDef.FIXED_SIZE
        return cls(bus_id=bus_id, address_mode=address_mode, address=address, reg_addr_width=reg_addr_width, auto_increment=auto_increment, flags=flags, _pad=_pad, clock_max_hz=clock_max_hz, name=name, reg_count=reg_count, registers=items)

@dataclass
class DevAttachResponse:
    status: int = 0
    dev_id: int = 0
    TYPE: ClassVar[int] = 0x20
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.dev_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevAttachResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevAttachResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, dev_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, dev_id=dev_id)

@dataclass
class DevDetachRequest:
    bus_id: int = 0
    dev_id: int = 0
    TYPE: ClassVar[int] = 0x21
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevDetachRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevDetachRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id)

@dataclass
class DevDetachResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x21
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevDetachResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevDetachResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class DevWriteRegsRequest:
    bus_id: int = 0
    dev_id: int = 0
    start_reg: int = 0
    count: int = 0
    data: bytes = b''
    TYPE: ClassVar[int] = 0x22
    FORMAT: ClassVar[str] = "<BBHH"
    FIXED_SIZE: ClassVar[int] = 6

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.start_reg, self.count)
        out += self.data
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevWriteRegsRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevWriteRegsRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, start_reg, count = struct.unpack_from(cls.FORMAT, data, 0)
        blob = bytes(data[cls.FIXED_SIZE:cls.FIXED_SIZE + count])
        return cls(bus_id=bus_id, dev_id=dev_id, start_reg=start_reg, count=count, data=blob)

@dataclass
class DevWriteRegsResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x22
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevWriteRegsResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevWriteRegsResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class DevReadRegsRequest:
    bus_id: int = 0
    dev_id: int = 0
    start_reg: int = 0
    count: int = 0
    TYPE: ClassVar[int] = 0x23
    FORMAT: ClassVar[str] = "<BBHH"
    FIXED_SIZE: ClassVar[int] = 6

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.start_reg, self.count)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevReadRegsRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevReadRegsRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, start_reg, count = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id, start_reg=start_reg, count=count)

@dataclass
class DevReadRegsResponse:
    status: int = 0
    count: int = 0
    data: bytes = b''
    TYPE: ClassVar[int] = 0x23
    FORMAT: ClassVar[str] = "<BH"
    FIXED_SIZE: ClassVar[int] = 3

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.count)
        out += self.data
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "DevReadRegsResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'DevReadRegsResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, count = struct.unpack_from(cls.FORMAT, data, 0)
        blob = bytes(data[cls.FIXED_SIZE:cls.FIXED_SIZE + count])
        return cls(status=status, count=count, data=blob)

@dataclass
class FaultSetRequest:
    bus_id: int = 0
    dev_id: int = 0
    fault_type: int = 0
    trigger: int = 0
    trigger_reg: int = 0
    repeat_count: int = 0
    trigger_n: int = 0
    param_a: int = 0
    param_b: int = 0
    probability_pct: int = 0
    _pad: int = 0
    _pad2: int = 0
    TYPE: ClassVar[int] = 0x30
    FORMAT: ClassVar[str] = "<BBBBHHIIIBBH"
    FIXED_SIZE: ClassVar[int] = 24

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.fault_type, self.trigger, self.trigger_reg, self.repeat_count, self.trigger_n, self.param_a, self.param_b, self.probability_pct, self._pad, self._pad2)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultSetRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultSetRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, fault_type, trigger, trigger_reg, repeat_count, trigger_n, param_a, param_b, probability_pct, _pad, _pad2 = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id, fault_type=fault_type, trigger=trigger, trigger_reg=trigger_reg, repeat_count=repeat_count, trigger_n=trigger_n, param_a=param_a, param_b=param_b, probability_pct=probability_pct, _pad=_pad, _pad2=_pad2)

@dataclass
class FaultSetResponse:
    status: int = 0
    fault_id: int = 0
    TYPE: ClassVar[int] = 0x30
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.fault_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultSetResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultSetResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, fault_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, fault_id=fault_id)

@dataclass
class FaultClearRequest:
    bus_id: int = 0
    dev_id: int = 0
    fault_id: int = 0
    TYPE: ClassVar[int] = 0x31
    FORMAT: ClassVar[str] = "<BBB"
    FIXED_SIZE: ClassVar[int] = 3

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.fault_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultClearRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultClearRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, fault_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id, fault_id=fault_id)

@dataclass
class FaultClearResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x31
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultClearResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultClearResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class FaultListRequest:
    bus_id: int = 0
    dev_id: int = 0
    TYPE: ClassVar[int] = 0x32
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultListRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultListRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id)

@dataclass
class FaultListResponse:
    status: int = 0
    count: int = 0
    faults: List[FaultInfo] = field(default_factory=list)
    TYPE: ClassVar[int] = 0x32
    FORMAT: ClassVar[str] = "<BB"
    FIXED_SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.count)
        for item in self.faults:
            out += item.pack()
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "FaultListResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'FaultListResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, count = struct.unpack_from(cls.FORMAT, data, 0)
        items = []
        off = cls.FIXED_SIZE
        for _ in range(count):
            items.append(FaultInfo.unpack(data[off:]))
            off += FaultInfo.FIXED_SIZE
        return cls(status=status, count=count, faults=items)

@dataclass
class TraceStartRequest:
    bus_id: int = 0
    flags: int = 0
    _pad: int = 0
    max_bytes: int = 0
    TYPE: ClassVar[int] = 0x40
    FORMAT: ClassVar[str] = "<BBHI"
    FIXED_SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.flags, self._pad, self.max_bytes)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "TraceStartRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'TraceStartRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, flags, _pad, max_bytes = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, flags=flags, _pad=_pad, max_bytes=max_bytes)

@dataclass
class TraceStartResponse:
    status: int = 0
    TYPE: ClassVar[int] = 0x40
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "TraceStartResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'TraceStartResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status)

@dataclass
class TraceStopRequest:
    bus_id: int = 0
    TYPE: ClassVar[int] = 0x41
    FORMAT: ClassVar[str] = "<B"
    FIXED_SIZE: ClassVar[int] = 1

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "TraceStopRequest":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'TraceStopRequest needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id)

@dataclass
class TraceStopResponse:
    status: int = 0
    overflowed: int = 0
    _pad: int = 0
    captured_edges: int = 0
    dropped_edges: int = 0
    TYPE: ClassVar[int] = 0x41
    FORMAT: ClassVar[str] = "<BBHII"
    FIXED_SIZE: ClassVar[int] = 12

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.status, self.overflowed, self._pad, self.captured_edges, self.dropped_edges)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "TraceStopResponse":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'TraceStopResponse needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        status, overflowed, _pad, captured_edges, dropped_edges = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(status=status, overflowed=overflowed, _pad=_pad, captured_edges=captured_edges, dropped_edges=dropped_edges)

@dataclass
class EvtDevAccessPayload:
    bus_id: int = 0
    dev_id: int = 0
    access: int = 0
    value: int = 0
    reg: int = 0
    _pad: int = 0
    timestamp_us: int = 0
    TYPE: ClassVar[int] = 0x80
    FORMAT: ClassVar[str] = "<BBBBHHQ"
    FIXED_SIZE: ClassVar[int] = 16

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.access, self.value, self.reg, self._pad, self.timestamp_us)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "EvtDevAccessPayload":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'EvtDevAccessPayload needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, access, value, reg, _pad, timestamp_us = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id, access=access, value=value, reg=reg, _pad=_pad, timestamp_us=timestamp_us)

@dataclass
class EvtFaultFiredPayload:
    bus_id: int = 0
    dev_id: int = 0
    fault_id: int = 0
    fault_type: int = 0
    remaining: int = 0
    _pad: int = 0
    timestamp_us: int = 0
    TYPE: ClassVar[int] = 0x81
    FORMAT: ClassVar[str] = "<BBBBHHQ"
    FIXED_SIZE: ClassVar[int] = 16

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.dev_id, self.fault_id, self.fault_type, self.remaining, self._pad, self.timestamp_us)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "EvtFaultFiredPayload":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'EvtFaultFiredPayload needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, dev_id, fault_id, fault_type, remaining, _pad, timestamp_us = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(bus_id=bus_id, dev_id=dev_id, fault_id=fault_id, fault_type=fault_type, remaining=remaining, _pad=_pad, timestamp_us=timestamp_us)

@dataclass
class EvtTraceDataPayload:
    bus_id: int = 0
    overflowed: int = 0
    edge_count: int = 0
    chunk_index: int = 0
    edges: List[TraceEdge] = field(default_factory=list)
    TYPE: ClassVar[int] = 0x82
    FORMAT: ClassVar[str] = "<BBHI"
    FIXED_SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.bus_id, self.overflowed, self.edge_count, self.chunk_index)
        for item in self.edges:
            out += item.pack()
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "EvtTraceDataPayload":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'EvtTraceDataPayload needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        bus_id, overflowed, edge_count, chunk_index = struct.unpack_from(cls.FORMAT, data, 0)
        items = []
        off = cls.FIXED_SIZE
        for _ in range(edge_count):
            items.append(TraceEdge.unpack(data[off:]))
            off += TraceEdge.FIXED_SIZE
        return cls(bus_id=bus_id, overflowed=overflowed, edge_count=edge_count, chunk_index=chunk_index, edges=items)

@dataclass
class EvtErrorPayload:
    code: int = 0
    context_type: int = 0
    context_seq: int = 0
    bus_id: int = 0
    dev_id: int = 0
    _pad: int = 0
    _pad2: int = 0
    detail: int = 0
    timestamp_us: int = 0
    TYPE: ClassVar[int] = 0x83
    FORMAT: ClassVar[str] = "<BBBBBBHIQ"
    FIXED_SIZE: ClassVar[int] = 20

    def pack(self) -> bytes:
        out = struct.pack(self.FORMAT, self.code, self.context_type, self.context_seq, self.bus_id, self.dev_id, self._pad, self._pad2, self.detail, self.timestamp_us)
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "EvtErrorPayload":
        if len(data) < cls.FIXED_SIZE:
            raise ValueError(
                f'EvtErrorPayload needs at least {cls.FIXED_SIZE} bytes, got {len(data)}'
            )
        code, context_type, context_seq, bus_id, dev_id, _pad, _pad2, detail, timestamp_us = struct.unpack_from(cls.FORMAT, data, 0)
        return cls(code=code, context_type=context_type, context_seq=context_seq, bus_id=bus_id, dev_id=dev_id, _pad=_pad, _pad2=_pad2, detail=detail, timestamp_us=timestamp_us)

MESSAGE_NAMES: Dict[int, str] = {
    0x01: "PING",
    0x02: "INFO",
    0x03: "RESET",
    0x10: "BUS_CREATE",
    0x11: "BUS_DESTROY",
    0x12: "BUS_LIST",
    0x20: "DEV_ATTACH",
    0x21: "DEV_DETACH",
    0x22: "DEV_WRITE_REGS",
    0x23: "DEV_READ_REGS",
    0x30: "FAULT_SET",
    0x31: "FAULT_CLEAR",
    0x32: "FAULT_LIST",
    0x40: "TRACE_START",
    0x41: "TRACE_STOP",
    0x80: "EVT_DEV_ACCESS",
    0x81: "EVT_FAULT_FIRED",
    0x82: "EVT_TRACE_DATA",
    0x83: "EVT_ERROR",
}

def is_async(msg_type: int) -> bool:
    """Bit 7 marks an event. No lookup needed to route a frame."""
    return bool(msg_type & 0x80)
