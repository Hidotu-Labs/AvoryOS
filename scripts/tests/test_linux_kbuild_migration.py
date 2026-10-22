#!/usr/bin/env python3
"""Check baseline-specific Kbuild selection and floating-point flag handling."""
import importlib.util
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("kbuild", root / "scripts/linux/kbuild-subset.py")
kbuild = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kbuild)
with tempfile.TemporaryDirectory(prefix="kbuild-migration-") as tmp:
    tree = Path(tmp)
    (tree / "driver").mkdir()
    (tree / "driver/fpu.c").touch()
    ev = kbuild.Evaluator(tmp)
    assert ev.expand("$(call test-lt,1024,2048)") == "y"
    assert ev.expand("$(call test-lt,2048,1024)") == ""
    assert ev.expand("$(call test-lt,,2048)") == ""
    try:
        ev.expand("$(call test-lt,bad,2048)")
        raise AssertionError("malformed arithmetic must fail")
    except kbuild.MakeError:
        pass
    ev.vars["CC_FLAGS_FPU"] = ["-msse -msse2 -mhard-float", True]
    ev.vars["CFLAGS_fpu.o"] = ["$(CC_FLAGS_FPU)", False]
    ev.vars["CFLAGS_REMOVE_fpu.o"] = ["-mno-sse -mno-sse2", True]
    ev.vars["CFLAGS_old_name.o"] = ["-msse", True]
    rules = ev.collect_per_file_flags("driver", ["driver/fpu.c"])
    assert rules == [("driver/fpu.c", "-msse -msse2 -mhard-float")]
    assert ev.per_file_removals == [("driver/fpu.c", "-mno-sse -mno-sse2")]
    assert not ev.unknown
    ev.collect_per_file_flags("driver", ["driver/old_name.c"])
    assert any("has no source" in msg for msg in ev.unknown)
print("PASS: Kbuild comparisons, selected FPU flags/removals, stale flags and missing-source failure")
