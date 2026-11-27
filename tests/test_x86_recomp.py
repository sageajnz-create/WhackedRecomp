"""Regression tests for the x86 code generator. Requires Capstone."""

from pathlib import Path
import runpy
import unittest

from capstone import CS_ARCH_X86, CS_MODE_32, Cs


translate_insn = runpy.run_path(
    str(Path(__file__).resolve().parents[1] / "tools" / "x86_recomp.py")
)["translate_insn"]


class X87Tests(unittest.TestCase):
    def test_fxch_uses_the_encoded_stack_register(self):
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        for index in range(8):
            with self.subTest(index=index):
                instruction, = decoder.disasm(bytes((0xD9, 0xC8 + index)), 0)
                line, = translate_insn(instruction, {}, {}, {})
                self.assertIn(
                    f"ctx.fpu_reg(0) = ctx.fpu_reg({index}); ctx.fpu_reg({index}) = _t;",
                    line,
                )


if __name__ == "__main__":
    unittest.main()
