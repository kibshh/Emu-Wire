#!/usr/bin/env python3
"""Generate the firmware header and host SDK module from the protocol spec.

    python protocol/codegen.py            # regenerate both outputs
    python protocol/codegen.py --check    # fail if the committed output is
                                          # stale, naming the file (used by CI)
    python protocol/codegen.py --rules    # list the checks run against the
                                          # spec, and what each one catches

Reads protocol/protocol.yaml, merges the files it includes, validates the
result, and emits:

    firmware/src/protocol/messages.h
    sdk/emuwire/protocol.py

Validation runs before anything is written, so a spec that breaks an invariant
produces no output at all rather than output that is quietly wrong. The rules
live here, in RULES, rather than in the spec: a list in the spec could either
be authoritative — and then deleting a line would switch a check off — or a
copy that goes stale. `--rules` prints them with the reason each exists.

Neither output is ever hand-edited. Output is deterministic — no timestamps,
no dict iteration order that depends on anything but the source files — so a
drift check is a plain diff.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:
    sys.exit("codegen needs PyYAML:  pip install pyyaml")

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
ENTRY = ROOT / "protocol.yaml"

C_OUT = REPO / "firmware" / "src" / "protocol" / "messages.h"
PY_OUT = REPO / "sdk" / "emuwire" / "protocol.py"

WIDTH = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "u64": 8, "char": 1}
C_TYPE = {
    "u8": "uint8_t",
    "i8": "int8_t",
    "u16": "uint16_t",
    "i16": "int16_t",
    "u32": "uint32_t",
    "i32": "int32_t",
    "u64": "uint64_t",
    "char": "char",
}
PY_FMT = {
    "u8": "B",
    "i8": "b",
    "u16": "H",
    "i16": "h",
    "u32": "I",
    "i32": "i",
    "u64": "Q",
    "char": "s",
}

# Every key a field may carry. Anything else is a typo, and a typo that is
# ignored rather than reported produces a header that looks right and is
# missing an annotation.
FIELD_KEYS = {
    "name",  # required
    "type",  # required
    "count",  # fixed-length array
    "count_field",  # variable-length: names the sibling holding the count
    "enum",  # value is drawn from this enum
    "bitmask",  # value is a set of these flags
    "struct",  # with type: struct, names the struct
    "description",
    "value",  # frame fields only: a fixed constant such as MAGIC
}


# ---------------------------------------------------------------------------
# Load
# ---------------------------------------------------------------------------


def load() -> dict[str, Any]:
    """Merge the spec. A key defined in two files is an error, not an override."""
    root = yaml.safe_load(ENTRY.read_text(encoding="utf-8"))
    merged: dict[str, Any] = {}
    origin: dict[tuple[str, str], str] = {}
    problems: list[str] = []

    for inc in root["includes"]:
        path = ROOT / inc
        if not path.exists():
            problems.append(f"{ENTRY.name}: include not found: {inc}")
            continue
        part = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for top, body in part.items():
            if not isinstance(body, dict):
                merged[top] = body
                continue
            merged.setdefault(top, {})
            for key, value in body.items():
                if key in merged[top]:
                    problems.append(
                        f"duplicate '{top}.{key}' defined in {inc} and {origin[(top, key)]}"
                    )
                merged[top][key] = value
                origin[(top, key)] = inc

    spec = {**root, **merged}

    # bus_protocol is built from buses/*.yaml rather than written by hand, so a
    # protocol cannot reach the wire without its pin roles and fault support.
    for name, rule in root.get("derived_enums", {}).items():
        if name in spec.get("enums", {}):
            problems.append(f"'{name}' is derived but also defined by hand")
        source = spec[rule["from"]]
        spec["enums"][name] = {
            "type": rule["type"],
            "derived_from": rule["from"],
            "values": {k: {"value": v["id"]} for k, v in source.items()},
        }

    spec["_problems"] = problems
    return spec


# ---------------------------------------------------------------------------
# Validate — the rules protocol.yaml declares
# ---------------------------------------------------------------------------


def fixed_size(fields: list[dict], spec: dict[str, Any]) -> int | None:
    """Byte size, or None if the field list has a variable-length tail.

    Also returns None for a field whose type or struct name is unknown.
    Validation reports those separately; this must not raise, or the first
    bad field would abort the run before the rest were reported.
    """
    total = 0
    for f in fields:
        if "count_field" in f:
            return None
        ftype = f.get("type")
        if ftype == "struct":
            if f.get("struct") not in spec["structs"]:
                return None
            sub = fixed_size(spec["structs"][f["struct"]]["fields"], spec)
            if sub is None:
                return None
            total += sub
        elif ftype in WIDTH:
            total += WIDTH[ftype] * f.get("count", 1)
        else:
            return None
    return total


# What validate() enforces, and why each one matters. This is the only list —
# protocol.yaml deliberately does not repeat it. A copy there could either
# switch a check off when a line was deleted, or go stale; neither is useful.
# Printed by --rules.
RULES = {
    "files and merging": {
        "includes_exist": "an include listed in protocol.yaml that is not on disk",
        "no_duplicate_keys_across_files": "a key defined twice, silently overriding",
        "derived_enums_not_hand_defined": "an enum both derived and written out",
    },
    "fields": {
        "field_has_name_and_type": "a field missing either",
        "field_keys_known": "`enumm:` instead of `enum:`, which would be ignored in silence",
        "field_types_known": "a width that does not exist",
        "struct_fields_name_a_struct": "type: struct with no struct named",
        "references_resolve": "an enum, bitmask or struct name that does not exist",
        "reference_widths_match": "a u16 field carrying a u8 enum, or the reverse",
        "count_field_exists": "a variable-length field whose count is not a sibling",
        "variable_field_is_last": "nothing can follow one, its end is unknown",
    },
    "messages": {
        "unique_message_ids": "two messages sharing an id",
        "ids_within_group_range": "an id outside its group's reserved range",
        "async_bit_matches_group": "bit 7 disagreeing with `group: async`",
        "command_declares_request": "a command with no request, so the host cannot build it",
        "response_starts_with_status": "a command whose failure mode is unreadable",
        "async_has_no_request": "an event modelled as if it were a command",
        "repeated_structs_fixed_size": "variable-length records in a DMA ring buffer",
    },
    "values": {
        "enum_values_unique_and_in_range": "two labels sharing a value, or one too wide",
        "bitmask_bits_unique_and_in_range": "two flags sharing a bit, or one too wide",
        "constants_fit_declared_width": "a sentinel that does not fit its type",
    },
    "buses": {
        "bus_ids_unique": "two protocols claiming the same wire value",
        "bus_pins_match_bus_create": "a pin role map that has drifted from the message",
        "bus_faults_exist": "a supported_faults entry that is not a fault_type",
        "bus_address_modes_exist": "an address_modes entry that is not an address_mode",
        "bus_declares_address_modes": "a bus with no way to identify a device",
        "address_modes_claimed_by_a_bus": "an address_mode value no bus accepts",
    },
}


def print_rules() -> None:
    total = sum(len(g) for g in RULES.values())
    print(f"{total} checks run before anything is emitted.\n")
    for group, rules in RULES.items():
        print(f"{group}:")
        width = max(len(n) for n in rules)
        for name, why in rules.items():
            print(f"  {name:<{width}}  {why}")
        print()


def validate(spec: dict[str, Any]) -> list[str]:
    errs: list[str] = list(spec["_problems"])
    enums = set(spec["enums"])
    bitmasks = set(spec["bitmasks"])
    structs = set(spec["structs"])

    def check_fields(fields: list[dict], where: str) -> None:
        # Required keys first. Everything below dereferences name and type, so
        # a field missing one must be reported and skipped rather than crash
        # the run and hide whatever else is wrong.
        for i, f in enumerate(fields):
            for required in ("name", "type"):
                if required not in f:
                    errs.append(f"{where}: field {i} has no '{required}' key: {f}")
        fields = [f for f in fields if "name" in f and "type" in f]

        siblings = {f["name"] for f in fields}
        for f in fields:
            # A key codegen does not recognise would otherwise be ignored in
            # silence: `enumm:` instead of `enum:` still emits a valid header,
            # just without the enum. Reject it.
            unknown = sorted(set(f) - FIELD_KEYS)
            if unknown:
                errs.append(
                    f"{where}.{f.get('name', '?')}: unknown key(s) {unknown}. "
                    f"Valid keys are {sorted(FIELD_KEYS)}"
                )
            if f["type"] not in WIDTH and f["type"] != "struct":
                errs.append(f"{where}.{f['name']}: unknown type '{f['type']}'")
            if f["type"] == "struct" and "struct" not in f:
                errs.append(f"{where}.{f['name']}: type is 'struct' but no struct name is given")
            for key, pool in (("enum", enums), ("bitmask", bitmasks), ("struct", structs)):
                if key in f and f[key] not in pool:
                    errs.append(f"{where}.{f['name']}: no such {key} '{f[key]}'")

            # `type` is the width on the wire; `enum`/`bitmask` say how to read
            # those bytes. They must agree, or the field reserves a different
            # number of bytes than the values it carries were defined for.
            for key, table in (("enum", spec["enums"]), ("bitmask", spec["bitmasks"])):
                if key in f and f[key] in table:
                    declared = table[f[key]]["type"]
                    if f["type"] != declared:
                        errs.append(
                            f"{where}.{f['name']}: field is {f['type']} but "
                            f"{key} '{f[key]}' is {declared}"
                        )
            if "count_field" in f and f["count_field"] not in siblings:
                errs.append(
                    f"{where}.{f['name']}: count_field '{f['count_field']}' "
                    f"is not a field of the same message"
                )

    for name, st in spec["structs"].items():
        check_fields(st["fields"], f"struct {name}")

    # The frame header is described the same way as any other field list, so
    # it gets the same checks. Nothing else reads it — the frame layout is
    # emitted from the individual constants above it — which is exactly why a
    # typo here would otherwise sit undetected.
    check_fields(spec["frame"]["fields"], "frame")

    # --- buses -------------------------------------------------------------
    pin_fields = {
        f["name"] for f in spec["messages"]["BUS_CREATE"]["request"] if f["name"].endswith("_pin")
    }
    fault_types = set(spec["enums"]["fault_type"]["values"])
    addr_modes = set(spec["enums"]["address_mode"]["values"])
    claimed_modes: set = set()
    bus_ids: dict[int, str] = {}

    for name, bus in spec["buses"].items():
        if bus["id"] in bus_ids:
            errs.append(f"bus id {bus['id']:#04x} claimed by both {name} and {bus_ids[bus['id']]}")
        bus_ids[bus["id"]] = name
        if set(bus["pins"]) != pin_fields:
            errs.append(
                f"bus {name}: pins {sorted(bus['pins'])} do not match "
                f"BUS_CREATE pin fields {sorted(pin_fields)}"
            )
        modes = bus.get("address_modes") or []
        if not modes:
            errs.append(f"bus {name}: declares no address_modes, so DEV_ATTACH has no valid value")
        for m in modes:
            if m not in addr_modes:
                errs.append(f"bus {name}: '{m}' is not an address_mode")
        claimed_modes |= set(modes)
        for f in bus.get("supported_faults", []):
            if f not in fault_types:
                errs.append(f"bus {name}: '{f}' is not a fault_type")

    for orphan in sorted(addr_modes - claimed_modes):
        errs.append(f"address_mode '{orphan}' is accepted by no bus")

    # --- messages ----------------------------------------------------------
    seen_ids: dict[int, str] = {}
    for name, msg in spec["messages"].items():
        mid = msg["id"]
        if mid in seen_ids:
            errs.append(f"message id {mid:#04x} used by both {name} and {seen_ids[mid]}")
        seen_ids[mid] = name

        group = msg["group"]
        if group not in spec["id_ranges"]:
            errs.append(f"{name}: unknown group '{group}'")
        else:
            lo, hi = spec["id_ranges"][group]
            if not lo <= mid <= hi:
                errs.append(f"{name}: id {mid:#04x} outside {group} range {lo:#04x}-{hi:#04x}")

        if bool(mid & 0x80) != (group == "async"):
            errs.append(
                f"{name}: id {mid:#04x} has bit 7 "
                f"{'set' if mid & 0x80 else 'clear'} but group is '{group}'"
            )

        if group == "async":
            if "payload" not in msg:
                errs.append(f"{name}: async message needs a payload")
            if "request" in msg or "response" in msg:
                errs.append(f"{name}: async message must not have request/response")
        else:
            # A command with no `request` key generates no request type at
            # all, so the host has no way to build the message. An empty
            # `request: []` is the way to say "no parameters".
            if "request" not in msg:
                errs.append(
                    f"{name}: command needs a request; use `request: []` if it takes no parameters"
                )
            if "response" not in msg:
                errs.append(f"{name}: command needs a response")
            elif msg["response"][0]["name"] != "status":
                errs.append(f"{name}: response must begin with 'status'")

        for section in ("request", "response", "payload"):
            if section not in msg:
                continue
            check_fields(msg[section], f"{name}.{section}")
            for f in msg[section]:
                # f.get, not f[...]: a field with a missing or misspelled
                # struct name has already been reported, and validation must
                # finish reporting rather than crash on the next bad field.
                if (
                    f.get("type") == "struct"
                    and "count_field" in f
                    and f.get("struct") in spec["structs"]
                    and fixed_size(spec["structs"][f["struct"]]["fields"], spec) is None
                ):
                    errs.append(
                        f"{name}.{section}.{f['name']}: repeated struct "
                        f"'{f['struct']}' is variable-length; ring-buffer and DMA "
                        f"handling require fixed-size records"
                    )
            # A variable-length field can only be last — nothing can follow it.
            for f in msg[section][:-1]:
                if "count_field" in f:
                    errs.append(f"{name}.{section}: '{f['name']}' is variable-length but not last")

    # --- bitmasks and constants -------------------------------------------
    for name, bm in spec["bitmasks"].items():
        bits: dict[int, str] = {}
        for label, entry in bm["bits"].items():
            bit = entry["bit"]
            if bit in bits:
                errs.append(f"bitmask {name}: bit {bit} used by both {label} and {bits[bit]}")
            bits[bit] = label
            if bit >= WIDTH[bm["type"]] * 8:
                errs.append(f"bitmask {name}.{label}: bit {bit} exceeds {bm['type']}")

    for name, const in spec.get("constants", {}).items():
        if const["value"] >= 1 << (WIDTH[const["type"]] * 8):
            errs.append(f"constant {name}: {const['value']:#x} does not fit {const['type']}")

    for name, en in spec["enums"].items():
        seen_vals: dict[int, str] = {}
        for label, entry in en["values"].items():
            v = entry["value"]
            if v in seen_vals:
                errs.append(f"enum {name}: {label} and {seen_vals[v]} share value {v:#x}")
            seen_vals[v] = label
            if v >= 1 << (WIDTH[en["type"]] * 8):
                errs.append(f"enum {name}.{label}: {v:#x} does not fit {en['type']}")

    return errs


# ---------------------------------------------------------------------------
# Naming
# ---------------------------------------------------------------------------


def camel(name: str) -> str:
    return "".join(p.capitalize() for p in name.split("_"))


def c_name(*parts: str) -> str:
    return "emuwire_" + "_".join(p.lower() for p in parts)


BANNER_C = """\
/*
 * DO NOT EDIT — generated from protocol/protocol.yaml by protocol/codegen.py
 *
 * Hand-editing this file makes the firmware and the host SDK disagree about
 * the wire format, and CI fails on the difference. Change the spec under
 * protocol/ and re-run:  python protocol/codegen.py
 */
"""

BANNER_PY = '''\
"""DO NOT EDIT — generated from protocol/protocol.yaml by protocol/codegen.py.

Hand-editing this file makes the host SDK and the firmware disagree about the
wire format, and CI fails on the difference. Change the spec under protocol/
and re-run:  python protocol/codegen.py
"""
'''


# ---------------------------------------------------------------------------
# Emit C
# ---------------------------------------------------------------------------


def emit_c(spec: dict[str, Any]) -> str:
    o: list[str] = [BANNER_C, "", "#ifndef EMUWIRE_MESSAGES_H", "#define EMUWIRE_MESSAGES_H", ""]
    o += ["#include <stdint.h>", "#include <stdbool.h>", ""]

    p, fr, crc = spec["protocol"], spec["frame"], spec["crc"]
    o += ["/* ---- protocol ---- */", f"#define EMUWIRE_PROTOCOL_VERSION {p['version']}u", ""]
    o += [
        "/* ---- frame ---- */",
        f"#define EMUWIRE_FRAME_MAGIC {fr['magic']:#04x}u",
        f"#define EMUWIRE_FRAME_HEADER_BYTES {fr['header_bytes']}u",
        f"#define EMUWIRE_FRAME_TRAILER_BYTES {fr['trailer_bytes']}u",
        f"#define EMUWIRE_FRAME_OVERHEAD_BYTES {fr['overhead_bytes']}u",
        "/* Validate LEN against this BEFORE reading a payload. */",
        f"#define EMUWIRE_MAX_PAYLOAD_BYTES {fr['max_payload_bytes']}u",
        f"#define EMUWIRE_SEQ_ASYNC {fr['seq']['async_value']}u",
        "",
    ]
    o += [
        f"/* ---- CRC: {crc['name']} ---- */",
        f"#define EMUWIRE_CRC_POLY {crc['polynomial']:#06x}u",
        f"#define EMUWIRE_CRC_INIT {crc['init']:#06x}u",
        f"#define EMUWIRE_CRC_XOROUT {crc['xor_out']:#06x}u",
        f"#define EMUWIRE_CRC_REFLECT_IN {str(crc['reflect_in']).lower()}",
        f"#define EMUWIRE_CRC_REFLECT_OUT {str(crc['reflect_out']).lower()}",
        '/* CRC of "123456789". crc16.c must reproduce this exactly. */',
        f"#define EMUWIRE_CRC_CHECK {crc['check']:#06x}u",
        "",
    ]

    if spec.get("constants"):
        o.append("/* ---- named sentinels ---- */")
        for name, c in spec["constants"].items():
            o.append(f"#define EMUWIRE_{name} (({C_TYPE[c['type']]}){c['value']:#x})")
        o.append("")

    o.append("/* ---- enums ---- */")
    for name, en in spec["enums"].items():
        note = f"  /* derived from {en['derived_from']}/ */" if "derived_from" in en else ""
        o.append(f"typedef enum {{{note}")
        for label, entry in en["values"].items():
            o.append(f"    EMUWIRE_{name.upper()}_{label} = {entry['value']:#04x},")
        o.append(f"}} {c_name(name)}_t;")
        o.append("")

    o.append("/* ---- bitmasks ---- */")
    for name, bm in spec["bitmasks"].items():
        for label, entry in bm["bits"].items():
            o.append(
                f"#define EMUWIRE_{name.upper()}_{label} "
                f"(({C_TYPE[bm['type']]})(1u << {entry['bit']}))"
            )
        o.append("")

    # Message ids are emitted by the enum loop above, from the derived
    # msg_type enum, so they are not repeated here.
    o += [
        "/* Bit 7 marks an asynchronous event: no lookup needed to route a frame. */",
        "#define EMUWIRE_MSG_IS_ASYNC(t) (((t) & 0x80u) != 0u)",
        "",
    ]

    def emit_struct(struct_name: str, fields: list[dict], comment: str = "") -> None:
        if comment:
            o.append(f"/* {comment} */")
        o.append("typedef struct __attribute__((packed)) {")
        tail: dict | None = None
        for f in fields:
            if "count_field" in f:
                tail = f
                continue
            note = (
                f"  /* {f['enum']} */"
                if "enum" in f
                else (f"  /* {f['bitmask']} */" if "bitmask" in f else "")
            )
            if f["type"] == "struct":
                o.append(f"    {c_name(f['struct'])}_t {f['name']};")
            elif f.get("count"):
                o.append(f"    {C_TYPE[f['type']]} {f['name']}[{f['count']}];{note}")
            else:
                o.append(f"    {C_TYPE[f['type']]} {f['name']};{note}")
        if tail is not None:
            ctype = (
                f"{c_name(tail['struct'])}_t" if tail["type"] == "struct" else C_TYPE[tail["type"]]
            )
            o.append(f"    /* {tail['count_field']} elements follow */")
            o.append(f"    {ctype} {tail['name']}[];")
        o.append(f"}} {struct_name}_t;")
        size = fixed_size([f for f in fields if "count_field" not in f], spec)
        o.append(
            f"_Static_assert(sizeof({struct_name}_t) == {size}, "
            f'"{struct_name}_t must be {size} bytes — packing changed");'
        )
        for f in fields:
            if "count_field" in f:
                continue
            off = fixed_size(fields[: fields.index(f)], spec)
            o.append(f"#define {struct_name.upper()}_{f['name'].upper()}_OFFSET {off}u")
        o.append("")

    o.append("/* ---- structs ---- */")
    for name, st in spec["structs"].items():
        emit_struct(c_name(name), st["fields"], st.get("description", "").strip().split("\n")[0])

    o.append("/* ---- message payloads ---- */")
    for name, msg in spec["messages"].items():
        for section in ("request", "response", "payload"):
            fields = msg.get(section)
            if not fields:
                continue
            emit_struct(c_name(name, section), fields)

    o += ["#endif /* EMUWIRE_MESSAGES_H */", ""]
    return "\n".join(o)


# ---------------------------------------------------------------------------
# Emit Python
# ---------------------------------------------------------------------------


def py_struct_fmt(fields: list[dict], spec: dict[str, Any]) -> str:
    fmt = ""
    for f in fields:
        if "count_field" in f:
            break
        if f["type"] == "struct":
            fmt += py_struct_fmt(spec["structs"][f["struct"]]["fields"], spec).lstrip("<")
        elif f["type"] == "char":
            fmt += f"{f.get('count', 1)}s"
        elif f.get("count"):
            fmt += PY_FMT[f["type"]] * f["count"]
        else:
            fmt += PY_FMT[f["type"]]
    return "<" + fmt


def emit_py(spec: dict[str, Any]) -> str:
    o: list[str] = [
        BANNER_PY,
        "",
        "from __future__ import annotations",
        "",
        "import struct",
        "from dataclasses import dataclass, field",
        "from enum import IntEnum",
        "from typing import Any, ClassVar, Dict, List, Optional, Tuple",
        "",
    ]

    p, fr, crc = spec["protocol"], spec["frame"], spec["crc"]
    o += [
        f"PROTOCOL_VERSION = {p['version']}",
        "",
        f"FRAME_MAGIC = {fr['magic']:#04x}",
        f"FRAME_HEADER_BYTES = {fr['header_bytes']}",
        f"FRAME_TRAILER_BYTES = {fr['trailer_bytes']}",
        f"FRAME_OVERHEAD_BYTES = {fr['overhead_bytes']}",
        f"MAX_PAYLOAD_BYTES = {fr['max_payload_bytes']}",
        f"SEQ_ASYNC = {fr['seq']['async_value']}",
        f"SEQ_MIN, SEQ_MAX = {fr['seq']['host_range'][0]}, {fr['seq']['host_range'][1]}",
        "",
    ]

    if spec.get("constants"):
        o.append("# Named sentinels. Several share a value but mean different things.")
        for name, c in spec["constants"].items():
            o.append(f"{name} = {c['value']:#x}")
        o.append("")

    # --- CRC, generated from the pinned parameters so it cannot drift -------
    o += [
        f"# {crc['name']}: poly={crc['polynomial']:#06x} init={crc['init']:#06x} "
        f"refin={crc['reflect_in']} refout={crc['reflect_out']} xorout={crc['xor_out']:#06x}",
        f"CRC_POLY = {crc['polynomial']:#06x}",
        f"CRC_INIT = {crc['init']:#06x}",
        f"CRC_XOROUT = {crc['xor_out']:#06x}",
        f"CRC_CHECK = {crc['check']:#06x}  # crc16(b'123456789')",
        "",
        "def _build_table() -> List[int]:",
        "    table = []",
        "    for byte in range(256):",
        "        reg = byte << 8",
        "        for _ in range(8):",
        "            reg = ((reg << 1) ^ CRC_POLY) & 0xFFFF if reg & 0x8000 else (reg << 1) & 0xFFFF",
        "        table.append(reg)",
        "    return table",
        "",
        "_CRC_TABLE = _build_table()",
        "",
        "def crc16(data: bytes) -> int:",
        '    """CRC over TYPE..PAYLOAD. Must agree byte for byte with crc16.c."""',
        "    reg = CRC_INIT",
        "    for byte in data:",
        "        reg = ((reg << 8) & 0xFFFF) ^ _CRC_TABLE[((reg >> 8) ^ byte) & 0xFF]",
        "    return reg ^ CRC_XOROUT",
        "",
    ]

    # --- framing -----------------------------------------------------------
    o += [
        "def encode_frame(msg_type: int, seq: int, payload: bytes = b'') -> bytes:",
        '    """Wrap a payload in a frame. The CRC covers TYPE..PAYLOAD, not MAGIC."""',
        "    if len(payload) > MAX_PAYLOAD_BYTES:",
        "        raise ValueError(",
        "            f'payload is {len(payload)} bytes, over the {MAX_PAYLOAD_BYTES} byte limit'",
        "        )",
        "    body = struct.pack('<BBH', msg_type, seq, len(payload)) + payload",
        "    return bytes([FRAME_MAGIC]) + body + struct.pack('<H', crc16(body))",
        "",
        "def decode_frame(buf: bytes) -> Tuple[Optional[Tuple[int, int, bytes]], int]:",
        '    """Find the first VALID frame in buf — not the first plausible one.',
        "",
        "    Returns ((msg_type, seq, payload), bytes_consumed), or (None, bytes_consumed)",
        "    when no complete frame is present yet. Never raises on malformed input and",
        "    never reads past the end of buf.",
        "",
        "    The magic byte can occur in noise and inside payloads, so a candidate is",
        "    only a guess until its CRC checks out. Scanning stops at the first",
        "    candidate that validates, and every earlier one is discarded.",
        "",
        "    A candidate whose claimed length runs past the end of buf cannot be",
        "    judged yet, so scanning continues past it — but its offset is kept as",
        "    `hold`, and bytes_consumed never goes beyond it. That way an incomplete",
        "    frame is retained for the next read, while a valid frame sitting after a",
        "    decoy is still delivered now. Returning at the decoy instead would stall",
        "    the reader until enough bytes arrived to disprove it, which on a link",
        "    where the peer is waiting for this reply is a deadlock rather than a",
        "    delay.",
        '    """',
        "    i = 0",
        "    hold: Optional[int] = None      # earliest candidate that might yet complete",
        "    while True:",
        "        start = buf.find(bytes([FRAME_MAGIC]), i)",
        "        if start < 0:",
        "            # No candidate left. Discard everything, except anything held.",
        "            return None, len(buf) if hold is None else hold",
        "        if len(buf) - start < FRAME_OVERHEAD_BYTES:",
        "            # Header incomplete: cannot even read the length yet.",
        "            return None, start if hold is None else hold",
        "        msg_type, seq, length = struct.unpack_from('<BBH', buf, start + 1)",
        "        if length > MAX_PAYLOAD_BYTES:",
        "            i = start + 1              # implausible length: this magic was noise",
        "            continue",
        "        end = start + FRAME_OVERHEAD_BYTES + length",
        "        if len(buf) < end:",
        "            if hold is None:",
        "                hold = start           # keep it, but keep looking",
        "            i = start + 1",
        "            continue",
        "        body = buf[start + 1:end - FRAME_TRAILER_BYTES]",
        "        got = struct.unpack_from('<H', buf, end - FRAME_TRAILER_BYTES)[0]",
        "        if got != crc16(body):",
        "            i = start + 1              # bad CRC: rescan from the next byte,",
        "            continue                   # never trust the length that failed",
        "        return (msg_type, seq, bytes(buf[start + FRAME_HEADER_BYTES:end - FRAME_TRAILER_BYTES])), end",
        "",
    ]

    # --- enums -------------------------------------------------------------
    for name, en in spec["enums"].items():
        o.append(f"class {camel(name)}(IntEnum):")
        for label, entry in en["values"].items():
            o.append(f"    {label} = {entry['value']:#04x}")
        o.append("")

    # --- status messages ---------------------------------------------------
    o.append("# Message templates, so no bare status code ever reaches a user.")
    o.append("STATUS_MESSAGES: Dict[int, str] = {")
    for label, entry in spec["enums"]["status"]["values"].items():
        text = entry.get("message", "").replace("\\", "\\\\").replace('"', '\\"')
        o.append(f'    Status.{label}: "{text}",')
    o.append("}")
    o.append("")

    # --- bitmasks ----------------------------------------------------------
    for name, bm in spec["bitmasks"].items():
        o.append(f"class {camel(name)}(IntEnum):")
        for label, entry in bm["bits"].items():
            o.append(f"    {label} = 1 << {entry['bit']}")
        o.append("")

    # --- per-bus capability tables ----------------------------------------
    o.append("# Per-bus semantics, so the SDK can reject a request locally with a")
    o.append("# specific message instead of sending one the board will refuse.")
    o.append("BUS_PINS: Dict[int, Dict[str, Optional[str]]] = {")
    for name, bus in spec["buses"].items():
        pins = ", ".join(f'"{k}": {v!r}' for k, v in bus["pins"].items())
        o.append(f"    BusProtocol.{name}: {{{pins}}},")
    o.append("}")
    o.append("BUS_SUPPORTED_FAULTS: Dict[int, Tuple[int, ...]] = {")
    for name, bus in spec["buses"].items():
        faults = ", ".join(f"FaultType.{f}" for f in bus.get("supported_faults", []))
        o.append(f"    BusProtocol.{name}: ({faults},),")
    o.append("}")
    o.append("BUS_ADDRESS_MODES: Dict[int, Tuple[int, ...]] = {")
    for name, bus in spec["buses"].items():
        modes = ", ".join(f"AddressMode.{m}" for m in bus.get("address_modes", []))
        o.append(f"    BusProtocol.{name}: ({modes},),")
    o.append("}")
    reserved = spec["buses"].get("I2C", {}).get("reserved_addresses", [])
    o.append("# Reserved by the I2C specification; rejected before anything reaches the board.")
    o.append("I2C_RESERVED_ADDRESSES: Tuple[Tuple[int, int], ...] = (")
    for r in reserved:
        o.append(f"    ({r['first']:#04x}, {r['last']:#04x}),  # {r['reason']}")
    o.append(")")
    o.append("")

    # --- dataclasses -------------------------------------------------------
    def emit_dc(cls: str, fields: list[dict], type_id: int | None = None) -> None:
        fixed = [f for f in fields if "count_field" not in f]
        tail = next((f for f in fields if "count_field" in f), None)
        fmt = py_struct_fmt(fixed, spec)
        size = fixed_size(fixed, spec) or 0

        o.append("@dataclass")
        o.append(f"class {cls}:")
        for f in fixed:
            if f["type"] == "struct":
                o.append(f"    {f['name']}: {camel(f['struct'])}")
            elif f["type"] == "char":
                o.append(f"    {f['name']}: bytes = b''")
            else:
                o.append(f"    {f['name']}: int = 0")
        if tail is not None:
            if tail["type"] == "struct":
                o.append(
                    f"    {tail['name']}: List[{camel(tail['struct'])}] = field(default_factory=list)"
                )
            else:
                o.append(f"    {tail['name']}: bytes = b''")
        if not fields:
            o.append("    pass")
        if type_id is not None:
            o.append(f"    TYPE: ClassVar[int] = {type_id:#04x}")
        o.append(f'    FORMAT: ClassVar[str] = "{fmt}"')
        o.append(f"    FIXED_SIZE: ClassVar[int] = {size}")
        o.append("")

        # pack
        o.append("    def pack(self) -> bytes:")
        if fixed:
            args = []
            for f in fixed:
                if f["type"] == "char":
                    args.append(f"self.{f['name']}.ljust({f.get('count', 1)}, b'\\0')")
                else:
                    args.append(f"self.{f['name']}")
            o.append(f"        out = struct.pack(self.FORMAT, {', '.join(args)})")
        else:
            o.append("        out = b''")
        if tail is not None:
            if tail["type"] == "struct":
                o.append(f"        for item in self.{tail['name']}:")
                o.append("            out += item.pack()")
            else:
                o.append(f"        out += self.{tail['name']}")
        o.append("        return out")
        o.append("")

        # unpack
        o.append("    @classmethod")
        o.append(f'    def unpack(cls, data: bytes) -> "{cls}":')
        o.append("        if len(data) < cls.FIXED_SIZE:")
        o.append("            raise ValueError(")
        o.append(
            f"                f'{cls} needs at least {{cls.FIXED_SIZE}} bytes, got {{len(data)}}'"
        )
        o.append("            )")
        if fixed:
            names = ", ".join(f["name"] for f in fixed)
            trailing = "," if len(fixed) == 1 else ""
            o.append(f"        {names}{trailing} = struct.unpack_from(cls.FORMAT, data, 0)")
        if tail is not None:
            if tail["type"] == "struct":
                o.append("        items = []")
                o.append("        off = cls.FIXED_SIZE")
                o.append(f"        for _ in range({tail['count_field']}):")
                o.append(f"            items.append({camel(tail['struct'])}.unpack(data[off:]))")
                o.append(f"            off += {camel(tail['struct'])}.FIXED_SIZE")
                extra = f", {tail['name']}=items"
            else:
                o.append(
                    f"        blob = bytes(data[cls.FIXED_SIZE:cls.FIXED_SIZE + {tail['count_field']}])"
                )
                extra = f", {tail['name']}=blob"
        else:
            extra = ""
        if fixed:
            kwargs = ", ".join(f"{f['name']}={f['name']}" for f in fixed)
            o.append(f"        return cls({kwargs}{extra})")
        else:
            o.append(f"        return cls({extra.lstrip(', ')})")
        o.append("")

    for name, st in spec["structs"].items():
        emit_dc(camel(name), st["fields"])

    for name, msg in spec["messages"].items():
        for section in ("request", "response", "payload"):
            if section not in msg:
                continue
            emit_dc(camel(name) + camel(section), msg[section], msg["id"])

    o.append("MESSAGE_NAMES: Dict[int, str] = {")
    for name, msg in spec["messages"].items():
        o.append(f'    {msg["id"]:#04x}: "{name}",')
    o.append("}")
    o.append("")
    o.append("def is_async(msg_type: int) -> bool:")
    o.append('    """Bit 7 marks an event. No lookup needed to route a frame."""')
    o.append("    return bool(msg_type & 0x80)")
    o.append("")
    return "\n".join(o)


# ---------------------------------------------------------------------------


def main() -> int:
    # Raw, so the usage examples in the module docstring keep their line breaks
    # instead of being reflowed into one paragraph.
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if the committed output is stale (used by CI)",
    )
    ap.add_argument(
        "--rules",
        action="store_true",
        help="list the checks run against the spec, and what each one catches",
    )
    args = ap.parse_args()

    if args.rules:
        print_rules()
        return 0

    spec = load()
    errs = validate(spec)
    if errs:
        print(f"protocol spec is invalid ({len(errs)} problems):", file=sys.stderr)
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1

    outputs = {C_OUT: emit_c(spec), PY_OUT: emit_py(spec)}

    if args.check:
        stale = []
        for path, text in outputs.items():
            current = path.read_text(encoding="utf-8") if path.exists() else None
            if current != text:
                stale.append(path)
        if stale:
            print("Generated files are out of date:", file=sys.stderr)
            for path in stale:
                print(f"  - {path.relative_to(REPO)}", file=sys.stderr)
            print("\nRun:  python protocol/codegen.py", file=sys.stderr)
            return 1
        print(f"up to date: {', '.join(p.name for p in outputs)}")
        return 0

    for path, text in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(REPO)}  ({len(text.splitlines())} lines)")

    counts = (
        f"{len(spec['messages'])} messages, {len(spec['enums'])} enums, "
        f"{len(spec['bitmasks'])} bitmasks, {len(spec['structs'])} structs, "
        f"{len(spec.get('constants', {}))} constants"
    )
    print(f"spec valid: {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
