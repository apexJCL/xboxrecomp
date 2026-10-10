"""Two ways a function only ever exists as a value.

1. An immediate naming a body that ends in a tail jump rather than a ret.
   Every C++ EH handler thunk is `mov eax, <FuncInfo>; jmp __CxxFrameHandler`,
   and the function's SEH prologue pushes its address. The imm-ref pass asked
   for a ret and refused them. A tail jump to the exact start of a known
   function is now accepted. A jump that lands anywhere else is not.

2. A code pointer in a data table whose function needs more than 64
   instructions to reach its first terminator. The data-pointer pass capped
   its walk at 64, because a long walk through junk finds a jmp eventually.
   The cap is raised only when there is more evidence: a known function start
   beside the pointer in its table, and a ret, tail jmp or int3 right in front
   of the target.

Fixtures are synthetic: hand-assembled bytes, no game data.
"""
import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

TEXT = 0x10000
DATA = 0x20000


class Image:
    base_address = TEXT
    entry_point = TEXT

    def __init__(self, code, data=b"\0" * 16):
        self.blobs = {".text": (TEXT, code), ".data": (DATA, data)}
        self.image_size = DATA + len(data) - TEXT
        self.text = SimpleNamespace(name=".text", virtual_addr=TEXT,
                                    virtual_size=len(code), raw_size=len(code),
                                    executable=True)
        self.data = SimpleNamespace(name=".data", virtual_addr=DATA,
                                    virtual_size=len(data), raw_size=len(data),
                                    executable=True)
        self.sections = [self.text, self.data]

    def get_section_at_va(self, addr):
        for sec in self.sections:
            if sec.virtual_addr <= addr < sec.virtual_addr + sec.virtual_size:
                return sec
        return None

    def get_section_data(self, section):
        return self.blobs[section.name][1]

    def read_bytes_at_va(self, addr, size):
        sec = self.get_section_at_va(addr)
        if sec is None:
            return b""
        lo, blob = self.blobs[sec.name]
        return blob[addr - lo:addr - lo + size]

    def read_u32_at_va(self, addr):
        raw = self.read_bytes_at_va(addr, 4)
        return struct.unpack("<I", raw)[0] if len(raw) == 4 else None


def _detect(code, data=b"\0" * 16):
    image = Image(code, data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.text)
    engine.resync_jump_tables()
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.text])
    return det


# --- 1. immediates naming a tail-calling body ---------------------------------

def _eh_thunk_image(jump_into_middle=False, padding=0):
    """
        TEXT+0x00: push NAMED ; call HANDLER ; add esp, 4 ; ret    caller
        TEXT+0x10: HANDLER: push esi ; xor eax, eax ; pop esi ; ret
        TEXT+0x15: FUNCLET: mov ecx, [ebp-0x10] ; jmp HANDLER
        TEXT+0x1D: `padding` int3s
                   THUNK:   mov eax, 0x00012345 ; jmp HANDLER
    THUNK follows a jmp with no padding, so neither boundary pass sees it, and
    nothing calls or jumps to it. NAMED is THUNK, or the int3 in front of it.
    """
    handler = TEXT + 0x10
    funclet = TEXT + 0x15
    thunk = TEXT + 0x1D + padding
    named = thunk - 1 if padding else thunk
    target = handler + 1 if jump_into_middle else handler
    code = b"\x68" + struct.pack("<I", named)
    code += b"\xe8" + struct.pack("<i", handler - (TEXT + 10))
    code += b"\x83\xc4\x04\xc3"
    code += b"\x90" * (0x10 - len(code))
    code += b"\x56\x31\xc0\x5e\xc3"
    code += b"\x8b\x4d\xf0" + b"\xe9" + struct.pack("<i", handler - (funclet + 8))
    code += b"\xcc" * padding
    code += b"\xb8\x45\x23\x01\x00"
    code += b"\xe9" + struct.pack("<i", target - (thunk + 10))
    code += b"\xcc" * 16
    return code, named


def test_an_eh_handler_thunk_named_by_an_immediate_is_a_function():
    code, thunk = _eh_thunk_image()
    det = _detect(code)
    assert thunk in det.functions, sorted(hex(a) for a in det.functions)
    assert det.functions[thunk].detection_method == "imm_ref_target"
    assert det.functions[thunk].end == thunk + 10


def test_a_tail_jump_to_no_function_start_is_still_refused():
    # The jmp is the only thing vouching for the bytes, so it has to land on a
    # function start; one byte in is what data that happens to decode does.
    code, thunk = _eh_thunk_image(jump_into_middle=True)
    det = _detect(code)
    assert thunk not in det.functions


def test_an_immediate_naming_padding_before_a_thunk_is_refused():
    # The walk from the int3 runs into the thunk and reaches its jmp. It
    # started in padding, so the jmp vouches for nothing.
    code, named = _eh_thunk_image(padding=1)
    det = _detect(code)
    assert named not in det.functions


# --- 2. data-table pointers that need a long probe ----------------------------

LONG = 70           # instructions before the first terminator: over 64


def _table_image(neighbour=True, after=b""):
    """
        TEXT+0x00: call KNOWN ; ret                    makes KNOWN a function
        TEXT+0x10: KNOWN: xor eax, eax ; ret
        TEXT+0x13: <after>                             nothing, or a nop
        TARGET:    inc eax x LONG ; ret
        .data:     KNOWN, TARGET, 0, 0x28              a handler record
    TARGET opens with `inc eax`, which is no prologue the gap pass knows.
    """
    known = TEXT + 0x10
    code = b"\xe8" + struct.pack("<i", known - (TEXT + 5)) + b"\xc3"
    code += b"\x90" * (0x10 - len(code))
    code += b"\x31\xc0\xc3" + after
    target = TEXT + len(code)
    code += b"\x40" * LONG + b"\xc3" + b"\xcc" * 16
    first = known if neighbour else 0x7
    data = struct.pack("<IIII", first, target, 0, 0x28)
    return code, data, target


def test_a_long_handler_beside_a_known_function_is_found():
    code, data, target = _table_image()
    det = _detect(code, data)
    assert target in det.functions, sorted(hex(a) for a in det.functions)


def test_without_a_known_neighbour_the_cap_stays():
    code, data, target = _table_image(neighbour=False)
    det = _detect(code, data)
    assert target not in det.functions


def test_without_a_boundary_in_front_the_cap_stays():
    # A data word that lands in zero-filled or table bytes inside a code
    # section has no ret in front of it. Same neighbour, same body.
    code, data, target = _table_image(after=b"\x90")
    det = _detect(code, data)
    assert target not in det.functions


def test_a_short_handler_needs_no_context():
    # Pre-existing behaviour: within 64 instructions, no neighbour needed.
    known = TEXT + 0x10
    code = b"\xe8" + struct.pack("<i", known - (TEXT + 5)) + b"\xc3"
    code += b"\x90" * (0x10 - len(code))
    code += b"\x31\xc0\xc3"
    target = TEXT + len(code)
    code += b"\x40" * 8 + b"\xc3" + b"\xcc" * 16
    data = struct.pack("<IIII", 0x7, target, 0, 0x28)
    det = _detect(code, data)
    assert target in det.functions
