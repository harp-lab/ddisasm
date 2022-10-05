import ctypes
import platform
import unittest
from disassemble_reassemble_check import compile, cd, disassemble
from pathlib import Path
import gtirb


ex_asm_arm_dir = Path("./examples/") / "arm_asm_examples"


class ValueRegTests(unittest.TestCase):
    @unittest.skipUnless(
        platform.system() == "Linux", "This test is linux only."
    )
    def test_value_reg_arm32(self):
        """
        Test that value_reg computes correct values.
        """
        binary = "ex"
        with cd(ex_asm_arm_dir / "ex_value_reg"):
            self.assertTrue(
                compile(
                    "arm-linux-gnueabihf-gcc",
                    "arm-linux-gnueabihf-g++",
                    "-O0",
                    [],
                    "qemu-arm -L /usr/arm-linux-gnueabihf",
                )
            )
            self.assertTrue(
                disassemble(
                    binary,
                    format="--ir",
                    strip=False,
                    extra_args=["--with-souffle-relations"],
                )[0]
            )

            ir_library = gtirb.IR.load_protobuf(binary + ".gtirb")
            m = ir_library.modules[0]

            fun = [s for s in m.symbols if s.name == "fun"][0]
            fun_block = fun.referent

            values_symbols = list(
                map(
                    lambda x: [
                        s for s in m.symbols if s.name == "value." + str(x)
                    ][0],
                    range(1, 11),
                )
            )
            values = list(
                map(
                    lambda x: int.from_bytes(x._payload.contents, "little"),
                    values_symbols,
                )
            )

            points = [
                edge.source.address + edge.source.size - 8
                for edge in fun_block.incoming_edges
                if edge.label.type == gtirb.Edge.Type.Call
            ]
            points.sort()

            assert len(values) == len(points)

            points_to_values = dict(zip(points, values))

            value_regs = (
                m.aux_data["souffleOutputs"]
                .data["value_reg"][1]
                .strip()
                .split("\n")
            )

            for tupl in value_regs:
                tupl = tupl.split("\t")

                ea0 = int(tupl[0], 0)
                ea = ea0 - (ea0 & 1)
                val = int(tupl[5], 0)

                if ea in points_to_values:
                    baseline = ctypes.c_int32(points_to_values[ea]).value
                    if val != baseline:
                        self.fail(f"{ea:#x}: {val} != {baseline}")


if __name__ == "__main__":
    unittest.main()