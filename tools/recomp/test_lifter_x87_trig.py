"""x87 trig ops, C2, and the `fnstsw ax; sahf; jcc` branch.

The x87 reduces FSIN/FCOS/FSINCOS/FPTAN operands with a 66-bit pi and, for
|x| >= 2^63, leaves st0 alone, pushes nothing and sets C2. The lifts call the
recomp_x87_* helpers in recomp_types.h (tests/x87_trig checks their values).
What is checked here is the lifter's side: the emitted calls, the conditional
push, fprem clearing C2, and the jcc after sahf reading the AH image sahf
loaded rather than g_fp_cmp. Before that, MSVC's CRT idioms `fsin; fnstsw ax;
sahf; jp reduce` and `fprem; fnstsw ax; sahf; jp loop` could not see C2 at
all, and the fmod loop spun forever after an unordered compare.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, _make_condition, lift_basic_block
from . import config
from .translator import FP_STACK_MACROS, FunctionTranslator

_ROOT = Path(__file__).resolve().parents[2]


def _insn(address, size, mnemonic, op_str="", operands=(), **attrs):
    instruction = Instruction(address, size, mnemonic, op_str, "")
    instruction.operands = list(operands)
    for key, value in attrs.items():
        setattr(instruction, key, value)
    return instruction


def _lift(mnemonic):
    return "\n".join(Lifter().lift_instruction(_insn(0, 2, mnemonic)))


def _ax():
    return Operand(type="reg", reg="ax")


class TrigEmitTest(unittest.TestCase):
    def test_fsin_and_fcos_call_the_pi66_helpers(self):
        self.assertIn("recomp_x87_fsin(&fp_top(), &g_fp_cc);", _lift("fsin"))
        self.assertIn("recomp_x87_fcos(&fp_top(), &g_fp_cc);", _lift("fcos"))
        self.assertNotIn("= sin(", _lift("fsin"))
        self.assertNotIn("RECOMP_FP_PC", _lift("fsin") + _lift("fptan"))

    def test_fptan_pushes_what_the_helper_hands_back_only_when_in_range(self):
        # 1.0 in range, the NaN for inf/NaN (tests/x87_trig checks the values).
        out = _lift("fptan")
        self.assertIn("if (recomp_x87_fptan(&fp_top(), &_p, &g_fp_cc)) fp_push(_p);", out)

    def test_fsincos_replaces_then_pushes_the_cosine(self):
        out = _lift("fsincos")
        self.assertIn("if (recomp_x87_fsincos(&fp_top(), &_c, &g_fp_cc)) fp_push(_c);",
                      out)

    def test_fprem_and_fprem1_clear_c2(self):
        for mnemonic, fn in (("fprem", "fmod"), ("fprem1", "remainder")):
            with self.subTest(mnemonic=mnemonic):
                out = _lift(mnemonic)
                self.assertIn(f"fp_top() = {fn}(fp_top(), fp_st1());", out)
                self.assertIn("g_fp_cc &= (uint16_t)~0x0400u;", out)


class SahfTest(unittest.TestCase):
    def test_sahf_snapshots_ah(self):
        self.assertIn("_fa = (eax >> 8) & 0xFFu;", _lift("sahf"))

    def test_conditions_after_sahf_read_the_snapshot(self):
        for jcc, expected in (("jp", "(_fa & 0x04u)"), ("je", "(_fa & 0x40u)"),
                              ("jb", "(_fa & 0x01u)"), ("ja", "!(_fa & 0x41u)"),
                              ("jbe", "(_fa & 0x41u)"), ("jnp", "!(_fa & 0x04u)")):
            with self.subTest(jcc=jcc):
                expr = _make_condition(jcc, "sahf", [])[0]
                self.assertIn(expected, expr)
                self.assertNotIn("g_fp_cmp", expr)

    def test_fcomi_still_reads_the_compare(self):
        self.assertIn("g_fp_cmp == 2", _make_condition("jp", "fucomip", [])[0])

    def test_sahf_writes_cf_when_the_function_tracks_it(self):
        lifter = Lifter()
        lifter.needs_cf = True
        out = "\n".join(lifter.lift_instruction(_insn(0, 1, "sahf")))
        self.assertIn("_cf = (int)(_fa & 1u);", out)
        self.assertNotIn("_cf", _lift("sahf"))

    def test_lahf_after_sahf_rebuilds_sf_zf_pf_cf(self):
        """`sahf; lahf` hands back the AH sahf loaded, sign bit included."""
        out = _block([_insn(0, 2, "fnstsw", "ax", [_ax()]), _insn(2, 1, "sahf"),
                      _insn(3, 1, "lahf")])
        for term in ("((_fa & 0x80u) /* sahf */) ? 0x80u",
                     "((_fa & 0x40u) /* sahf */) ? 0x40u",
                     "((_fa & 0x04u) /* sahf */) ? 0x04u",
                     "((_fa & 0x01u) /* sahf */) ? 0x01u"):
            self.assertIn(term, out)


def _block(instructions):
    stmts = lift_basic_block(Lifter(), BasicBlock(start=0, instructions=instructions))[0]
    return "\n".join("    " + s for s in stmts)


# `fsin; fnstsw ax; sahf; jp reduce` (MSVC's _CIsin) and `fprem; fnstsw ax;
# sahf; jp loop` (its fmod), lifted, then run. The jp leaves the block, so it
# lifts to a call of sub_00000040, which records that it was taken.
_SOURCE = r"""
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
@MACROS@
static int taken;
static void sub_00000040(void) { taken = 1; }
static void trig(void) {
    uint32_t eax = 0, ebp = 0; uint32_t _fa = 0; (void)ebp; (void)_fa;
@TRIG@
}
static void rem(void) {
    uint32_t eax = 0, ebp = 0; uint32_t _fa = 0; (void)ebp; (void)_fa;
@REM@
}
static void cmp(void) {
    uint32_t eax = 0, ebp = 0; uint32_t _fa = 0; (void)ebp; (void)_fa;
@CMP@
}
static int run_trig(double x, int stale_cmp) {
    g_fp_top = 0; fp_push(x); g_fp_cmp = stale_cmp; g_fp_cc = 0; taken = 0;
    trig();
    return taken;
}
int main(void) {
    /* Out of range: C2 set, st0 kept, jp taken. The last compare was "equal",
     * which is what the old g_fp_cmp condition answered from. */
    if (!run_trig(18446744073709551616.0, 0)) return 10;
    if (fp_top() != 18446744073709551616.0) return 11;
    /* In range: C2 clear, jp not taken, even after an unordered compare. */
    if (run_trig(1.0, 2)) return 12;
    if (fp_top() != sin(1.0)) return 13;
    /* fmod(7, 2.5) after a NaN compare: one fprem, C2 clear, the loop exits. */
    g_fp_top = 0; fp_push(2.5); fp_push(7.0); g_fp_cmp = 2; g_fp_cc = 0x0400; taken = 0;
    rem();
    if (taken || fp_top() != 2.0) return 14;
    /* A compare still branches as before: 2.0 is not below 1.0, so no jb;
     * 1.0 below 2.0 takes it. */
    g_fp_top = 0; fp_push(1.0); fp_push(2.0); taken = 0; cmp();
    if (taken) return 15;
    g_fp_top = 0; fp_push(2.0); fp_push(1.0); taken = 0; cmp();
    if (!taken) return 16;
    puts("OK");
    return 0;
}
"""


def _sources():
    jp = dict(jump_target=0x40)
    trig = _block([
        _insn(0, 2, "fsin"),
        _insn(2, 2, "fnstsw", "ax", [_ax()]),
        _insn(4, 1, "sahf"),
        _insn(5, 2, "jp", "0x40", [Operand(type="imm", imm=0x40)], **jp),
    ])
    rem = _block([
        _insn(0, 2, "fprem"),
        _insn(2, 2, "fnstsw", "ax", [_ax()]),
        _insn(4, 1, "sahf"),
        _insn(5, 2, "jp", "0x40", [Operand(type="imm", imm=0x40)], **jp),
    ])
    cmp = _block([
        _insn(0, 2, "fcompp"),
        _insn(2, 2, "fnstsw", "ax", [_ax()]),
        _insn(4, 1, "sahf"),
        _insn(5, 2, "jb", "0x40", [Operand(type="imm", imm=0x40)], **jp),
    ])
    return trig, rem, cmp


def _compile_and_run(trig, rem, cmp):
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("C compiler unavailable")
    source = (_SOURCE.replace("@MACROS@", "\n".join(FP_STACK_MACROS))
              .replace("@TRIG@", trig).replace("@REM@", rem)
              .replace("@CMP@", cmp))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "sahf.c"
        path.write_text(source)
        exe = Path(tmp) / "sahf.exe"
        command = [cc, "-w", str(path), "-o", str(exe),
                   "-I", str(_ROOT / "templates" / "runtime"), "-I", str(_ROOT / "src")]
        if os.name != "nt":
            command.append("-lm")
        built = subprocess.run(command, capture_output=True, text=True)
        assert built.returncode == 0, built.stderr[-2000:]
        return subprocess.run([str(exe)], capture_output=True, text=True)


class SahfTranslatorTest(unittest.TestCase):
    """A function whose only flag setter is sahf still declares _fa."""

    _CONFIG_GLOBALS = (
        "_SECTIONS", "SECTIONS", "_configured_from",
        "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
        "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
    )

    def setUp(self):
        self._saved = {k: getattr(config, k) for k in self._CONFIG_GLOBALS}

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def test_sahf_only_function_declares_the_snapshot(self):
        base = 0x00011000
        image = bytes.fromhex("DFE09EC3")  # fnstsw ax; sahf; ret
        config._install(
            [config.Section(".text", base, len(image), 0, len(image), True)],
            entry_point=base, kernel_thunk_addr=base, origin="x87-sahf-test")
        db = {base: {"start": f"0x{base:08X}", "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        c = FunctionTranslator(image, db).translate_function(base, db[base])
        self.assertIn("_fa = (eax >> 8) & 0xFFu;", c)
        self.assertIn("uint32_t _fa = 0, _fb = 0;", c)


class SahfCompiledTest(unittest.TestCase):
    def test_crt_c2_idioms_branch_on_the_status_word(self):
        trig, rem, cmp = _sources()
        self.assertIn("(_fa & 0x04u) /* sahf */", trig)
        self.assertIn("(_fa & 0x01u) /* sahf */", cmp)
        result = _compile_and_run(trig, rem, cmp)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_negative_control_the_old_condition_fails(self):
        """The pre-fix `(g_fp_cmp == 2)` must fail the same program."""
        trig, rem, cmp = _sources()
        old = "(g_fp_cmp == 2) /* sahf */"
        trig = trig.replace("(_fa & 0x04u) /* sahf */", old)
        rem = rem.replace("(_fa & 0x04u) /* sahf */", old)
        result = _compile_and_run(trig, rem, cmp)
        self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
