"""Test fixtures for the host SDK.

These tests run with nothing installed and no hardware attached. The SDK is
not a package yet, so `sdk/` goes on the path and `emuwire.protocol` is
imported as a PEP 420 namespace package.
"""

from __future__ import annotations

import sys
from pathlib import Path
from types import ModuleType

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "sdk"))  # emuwire.protocol
sys.path.insert(0, str(REPO / "protocol"))  # codegen


@pytest.fixture(scope="session")
def proto() -> ModuleType:
    """The generated wire-protocol module."""
    import emuwire.protocol

    return emuwire.protocol


@pytest.fixture(scope="session")
def spec() -> dict:
    """The merged protocol spec, loaded by codegen's own loader.

    Not a copy and not a reimplementation — this calls the same load() that
    codegen calls. Parsing the YAML here instead would mean a second merge
    implementation (includes, duplicate detection, derived enums) that could
    disagree with the real one, and then a test could pass against a spec
    codegen never saw.

    The limit of that: these tests check the generated module against the
    spec *as codegen understood it*. If load() itself were wrong both sides
    would agree and both be wrong. What guards that is codegen's own
    validation, and the fact that the output has to compile and import.
    """
    import codegen

    return codegen.load()
