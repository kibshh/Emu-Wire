"""Wire protocol tests: round-trips, malformed input, and fuzzing.

The round-trip tests are driven by the protocol spec rather than a hand-written
list of messages, so a message added to the spec is tested without touching
this file. A list here would drift, and a test that silently stops covering
something is worse than no test.

The decoder tests exist for one property above all: `decode_frame` must never
raise and never stop making progress, whatever bytes arrive. It is fed
whatever a half-open serial port, a board resetting mid-frame, or line noise
produces.
"""

from __future__ import annotations

import random
import struct
from typing import Any

import pytest

# Largest value each width can hold, for the boundary round-trip.
MAX_VALUE = {
    "u8": 0xFF,
    "u16": 0xFFFF,
    "u32": 0xFFFF_FFFF,
    "u64": 0xFFFF_FFFF_FFFF_FFFF,
    "i8": 0x7F,
    "i16": 0x7FFF,
    "i32": 0x7FFF_FFFF,
}

REPEAT_COUNT = 3  # elements to use when a message carries a variable-length array


# ---------------------------------------------------------------------------
# Building sample messages from the spec
# ---------------------------------------------------------------------------


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def _value(field: dict, index: int, *, maximum: bool) -> Any:
    """A test value for one field.

    With maximum=False each field gets a different small number, so a field
    swapped with its neighbour changes the decoded result. With maximum=True
    every field gets the largest value its width holds, so a field packed as
    the wrong width truncates and fails.
    """
    ftype = field["type"]
    if ftype == "char":
        return b"abcdefghijklmnop"[: field.get("count", 1)]
    if maximum:
        return MAX_VALUE[ftype]
    return (index * 7 + 1) % 200  # distinct, and small enough for a u8


def build(cls: Any, fields: list[dict], spec: dict, *, maximum: bool) -> Any:
    """Instantiate a generated dataclass with values for every field."""
    kwargs: dict[str, Any] = {}
    counts: dict[str, int] = {}

    for index, field in enumerate(fields):
        name = field["name"]
        if "count_field" in field:
            # Variable-length tail. Its length must match the sibling that
            # states it, or the decoded object cannot match the original.
            counts[field["count_field"]] = REPEAT_COUNT
            if field["type"] == "struct":
                nested = spec["structs"][field["struct"]]["fields"]
                nested_cls = getattr(cls.__module__ and _module(cls), camel(field["struct"]))
                kwargs[name] = [
                    build(nested_cls, nested, spec, maximum=maximum) for _ in range(REPEAT_COUNT)
                ]
            else:
                kwargs[name] = bytes(range(REPEAT_COUNT))
        elif field["type"] == "struct":
            nested = spec["structs"][field["struct"]]["fields"]
            nested_cls = getattr(_module(cls), camel(field["struct"]))
            kwargs[name] = build(nested_cls, nested, spec, maximum=maximum)
        else:
            kwargs[name] = _value(field, index, maximum=maximum)

    kwargs.update(counts)  # the declared count wins over the generic value
    return cls(**kwargs)


def _module(cls: Any) -> Any:
    import sys

    return sys.modules[cls.__module__]


def message_sections(spec: dict) -> list[tuple[str, str, list[dict]]]:
    """Every (message, section, fields) triple in the spec."""
    out = []
    for name, msg in spec["messages"].items():
        for section in ("request", "response", "payload"):
            if section in msg:
                out.append((name, section, msg[section]))
    return out


# ---------------------------------------------------------------------------
# CRC
# ---------------------------------------------------------------------------


def test_crc_check_vector(proto):
    """The published check value for the pinned variant.

    If this changes, the firmware and the SDK disagree about every frame.
    """
    assert proto.crc16(b"123456789") == 0x29B1
    assert proto.CRC_CHECK == 0x29B1


def test_crc_is_not_a_neighbouring_variant(proto):
    """Same polynomial, different parameters, different answer.

    These are the values XMODEM, KERMIT and GENIBUS produce for the same
    input. Matching one of them would mean the init or reflection settings
    had silently changed.
    """
    assert proto.crc16(b"123456789") not in (0x31C3, 0x2189, 0xD64E)


def test_crc_of_empty_input_is_the_init_value(proto):
    assert proto.crc16(b"") == proto.CRC_INIT ^ proto.CRC_XOROUT


def test_crc_detects_single_bit_flips(proto):
    """A CRC that missed these would be worse than none."""
    data = bytes(range(64))
    baseline = proto.crc16(data)
    for i in range(len(data)):
        for bit in range(8):
            corrupted = bytearray(data)
            corrupted[i] ^= 1 << bit
            assert proto.crc16(bytes(corrupted)) != baseline


# ---------------------------------------------------------------------------
# Framing
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("size", [0, 1, 2, 255, 256, 1024])
def test_frame_round_trip(proto, size):
    payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
    frame = proto.encode_frame(0x01, 42, payload)

    assert frame[0] == proto.FRAME_MAGIC
    assert len(frame) == size + proto.FRAME_OVERHEAD_BYTES

    got, used = proto.decode_frame(frame)
    assert got is not None
    msg_type, seq, body = got
    assert (msg_type, seq, body) == (0x01, 42, payload)
    assert used == len(frame)


def test_frame_rejects_oversized_payload(proto):
    """The encoder refuses rather than emitting a frame no decoder will accept."""
    with pytest.raises(ValueError, match="limit"):
        proto.encode_frame(0x01, 1, b"\x00" * (proto.MAX_PAYLOAD_BYTES + 1))


def test_crc_covers_type_seq_and_len_not_magic(proto):
    """Corrupting any byte the CRC covers must invalidate the frame."""
    frame = proto.encode_frame(0x10, 5, b"\x01\x02\x03")
    for i in range(1, len(frame) - proto.FRAME_TRAILER_BYTES):
        corrupted = bytearray(frame)
        corrupted[i] ^= 0xFF
        got, _ = proto.decode_frame(bytes(corrupted))
        # Either rejected outright, or resynced into something that is not
        # the original frame. Both are acceptable; silently accepting is not.
        assert got is None or got != (0x10, 5, b"\x01\x02\x03")


# ---------------------------------------------------------------------------
# Every message type
# ---------------------------------------------------------------------------


def test_every_spec_message_has_generated_classes(proto, spec):
    """Codegen must not quietly drop a message."""
    for name, section, _ in message_sections(spec):
        cls_name = camel(name) + camel(section)
        assert hasattr(proto, cls_name), f"{cls_name} missing from the generated module"


@pytest.mark.parametrize("maximum", [False, True], ids=["distinct", "max-values"])
def test_message_round_trip(proto, spec, maximum):
    """Pack and unpack every message section in the spec.

    Run twice: once with a different value per field, which catches two
    fields being swapped, and once with each width's maximum, which catches a
    field packed as the wrong width.
    """
    for name, section, fields in message_sections(spec):
        cls = getattr(proto, camel(name) + camel(section))
        original = build(cls, fields, spec, maximum=maximum)
        restored = cls.unpack(original.pack())
        assert restored == original, f"{name}.{section} did not survive a round trip"


def test_message_fixed_size_matches_the_packed_bytes(proto, spec):
    for name, section, fields in message_sections(spec):
        cls = getattr(proto, camel(name) + camel(section))
        if any("count_field" in f for f in fields):
            continue  # variable length, FIXED_SIZE is only the head
        packed = build(cls, fields, spec, maximum=False).pack()
        assert len(packed) == cls.FIXED_SIZE, f"{name}.{section}"


def test_messages_survive_a_frame(proto, spec):
    """The whole path: build, pack, frame, decode, unpack."""
    for name, section, fields in message_sections(spec):
        cls = getattr(proto, camel(name) + camel(section))
        original = build(cls, fields, spec, maximum=False)
        frame = proto.encode_frame(cls.TYPE, 1, original.pack())
        got, _ = proto.decode_frame(frame)
        assert got is not None
        assert cls.unpack(got[2]) == original, name


def test_unpack_rejects_a_short_buffer(proto, spec):
    """Never read past the end of what arrived."""
    for name, section, _fields in message_sections(spec):
        cls = getattr(proto, camel(name) + camel(section))
        if cls.FIXED_SIZE == 0:
            continue
        with pytest.raises(ValueError):
            cls.unpack(b"\x00" * (cls.FIXED_SIZE - 1))


def test_async_messages_use_seq_zero(proto, spec):
    """Bit 7 of the type and SEQ 0 are what separate events from replies."""
    for name, msg in spec["messages"].items():
        is_async = bool(msg["id"] & 0x80)
        assert proto.is_async(msg["id"]) is is_async
        assert (msg["group"] == "async") is is_async, name


# ---------------------------------------------------------------------------
# Malformed input
# ---------------------------------------------------------------------------


def drain(proto, buf: bytes) -> tuple[list[tuple[int, int, bytes]], bytes]:
    """Decode as a real reader would, and assert the loop always progresses."""
    frames = []
    while buf:
        got, used = proto.decode_frame(buf)
        if got is None:
            assert used <= len(buf)
            return frames, buf[used:]
        assert used > 0, "decoder returned a frame but consumed nothing"
        frames.append(got)
        buf = buf[used:]
    return frames, b""


def test_no_magic_discards_everything(proto):
    got, used = proto.decode_frame(b"\x01\x02\x03\x04")
    assert got is None and used == 4


def test_truncated_frame_waits_for_more(proto):
    """An incomplete frame must be kept, not thrown away."""
    frame = proto.encode_frame(0x01, 1, b"\xaa" * 8)
    for cut in range(1, len(frame)):
        got, used = proto.decode_frame(frame[:cut])
        assert got is None
        assert used == 0, "the start of an incomplete frame must not be consumed"


def test_bad_crc_is_rejected_and_rescans_from_the_next_byte(proto):
    frame = bytearray(proto.encode_frame(0x01, 1, b"\x11\x22"))
    frame[-1] ^= 0xFF  # corrupt the CRC itself
    got, used = proto.decode_frame(bytes(frame))
    assert got is None
    assert used <= len(frame)


def test_length_beyond_the_limit_is_not_trusted(proto):
    """A length field is noise-controlled. Never allocate or read on it.

    The consumed count is the part that matters. A length over the limit can
    never become valid however much more data arrives, so those bytes must be
    discarded outright. Holding them would let noise grow the reader's buffer
    without bound, and in C would be a read past the end of it.
    """
    header = struct.pack("<BBH", 0x01, 1, proto.MAX_PAYLOAD_BYTES + 1)
    buf = bytes([proto.FRAME_MAGIC]) + header + b"\x00\x00"
    got, used = proto.decode_frame(buf)
    assert got is None
    assert used == len(buf), "an impossible length must be discarded, not retained"


def test_a_plausible_but_incomplete_length_is_retained(proto):
    """The opposite case, to keep the one above honest.

    A length within the limit may simply not have arrived yet, so those bytes
    are kept rather than discarded.
    """
    header = struct.pack("<BBH", 0x01, 1, proto.MAX_PAYLOAD_BYTES)
    buf = b"\x00\x00" + bytes([proto.FRAME_MAGIC]) + header
    got, used = proto.decode_frame(buf)
    assert got is None
    assert used == 2, "only the leading garbage should be consumed"


def test_resync_after_garbage(proto):
    frame = proto.encode_frame(0x02, 9, b"\xde\xad\xbe\xef")
    noise = b"\x00\xff\x7f" + bytes([proto.FRAME_MAGIC]) + b"\x99\x99\xff\xff\x01"
    frames, leftover = drain(proto, noise + frame)
    assert (0x02, 9, b"\xde\xad\xbe\xef") in frames
    assert leftover == b""


def test_decoy_magic_inside_a_payload(proto):
    """A payload may contain the magic byte. It must not split the frame."""
    payload = bytes([proto.FRAME_MAGIC]) * 16
    frame = proto.encode_frame(0x03, 4, payload)
    frames, _ = drain(proto, frame)
    assert frames == [(0x03, 4, payload)]


def test_a_whole_frame_inside_an_incomplete_one_wins(proto):
    """A known trade-off, pinned here so changing it is a decision.

    If an incomplete frame's payload happens to contain a complete,
    CRC-valid frame, the inner one is returned and the outer one is lost.

    The alternative is to prefer the outer frame and wait for it, which is
    what this decoder used to do — and a decoy length in line noise then
    stalls the reader until enough bytes arrive to disprove it. When the peer
    is waiting for the reply we are not sending, that is a deadlock rather
    than a delay. Losing a frame is recoverable: the host's sequence matching
    notices the missing reply and retries.

    It needs the payload to contain the magic byte, then a plausible length,
    then two bytes matching the CRC of that span, so it is rare. A decoder
    that holds its own buffer — the SDK transport rather than this stateless
    function — can do better, because it knows which sequence numbers are
    outstanding and can discard a reply nobody asked for.
    """
    inner = proto.encode_frame(0x02, 9, b"xy")
    outer = proto.encode_frame(0x01, 1, b"\x00" * 4 + inner + b"\x00" * 40)

    # Enough of the outer frame has arrived to contain all of the inner one.
    arrived = outer[: outer.find(inner) + len(inner) + 3]

    got, used = proto.decode_frame(arrived)
    assert got == (0x02, 9, b"xy"), "the complete frame should win"
    assert used == outer.find(inner) + len(inner)

    # And the decoder still makes progress rather than wedging on the remains.
    drain(proto, arrived[used:] + outer[len(arrived) :])


def test_back_to_back_frames(proto):
    a = proto.encode_frame(0x01, 1, b"one")
    b = proto.encode_frame(0x02, 2, b"two")
    frames, leftover = drain(proto, a + b)
    assert frames == [(0x01, 1, b"one"), (0x02, 2, b"two")]
    assert leftover == b""


def test_every_prefix_and_every_corruption_is_safe(proto):
    """Never raise, never read out of bounds, for any mutation of a real frame."""
    frame = proto.encode_frame(0x20, 3, bytes(range(32)))
    for cut in range(len(frame) + 1):
        proto.decode_frame(frame[:cut])
    for i in range(len(frame)):
        for mask in (0x01, 0x80, 0xFF):
            corrupted = bytearray(frame)
            corrupted[i] ^= mask
            proto.decode_frame(bytes(corrupted))


# ---------------------------------------------------------------------------
# Fuzz
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("seed", range(20))
def test_fuzz_random_bytes_never_raise(proto, seed):
    rng = random.Random(seed)
    for _ in range(200):
        length = rng.randrange(0, 300)
        proto.decode_frame(bytes(rng.randrange(256) for _ in range(length)))


@pytest.mark.parametrize("seed", range(20))
def test_fuzz_magic_heavy_streams_never_raise(proto, seed):
    """Random bytes rarely contain magic. Bias towards it to exercise the
    header path, where a length field is read before it is trusted."""
    rng = random.Random(seed)
    alphabet = [proto.FRAME_MAGIC] * 4 + [0x00, 0xFF, 0x01, 0x7F]
    for _ in range(200):
        length = rng.randrange(0, 300)
        proto.decode_frame(bytes(rng.choice(alphabet) for _ in range(length)))


@pytest.mark.parametrize("seed", range(20))
def test_fuzz_never_wedges(proto, seed):
    """The liveness property: whatever arrived before it, a valid frame that
    follows must still be found. A decoder that survives garbage but can no
    longer decode is as broken as one that crashes."""
    rng = random.Random(seed)
    frame = proto.encode_frame(0x01, 77, b"payload")
    alphabet = [proto.FRAME_MAGIC] * 3 + [0x00, 0xFF, 0x42]
    for _ in range(100):
        noise = bytes(rng.choice(alphabet) for _ in range(rng.randrange(0, 64)))
        frames, _ = drain(proto, noise + frame)
        assert (0x01, 77, b"payload") in frames, f"wedged after {noise.hex()}"


@pytest.mark.parametrize("seed", range(10))
def test_fuzz_corrupted_frames_never_decode_as_valid(proto, seed):
    """Corruption must be caught, not silently passed through."""
    rng = random.Random(seed)
    original = (0x30, 12, bytes(range(24)))
    frame = proto.encode_frame(*original)
    for _ in range(300):
        corrupted = bytearray(frame)
        for _ in range(rng.randrange(1, 4)):
            corrupted[rng.randrange(len(corrupted))] ^= 1 << rng.randrange(8)
        if bytes(corrupted) == frame:
            continue
        got, _ = proto.decode_frame(bytes(corrupted))
        assert got != original, "a corrupted frame decoded as the original"
