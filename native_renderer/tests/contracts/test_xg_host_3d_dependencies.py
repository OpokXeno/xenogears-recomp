#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "include" / "xg_host_3d.h"
SOURCE = ROOT / "src" / "core" / "xg_host_3d.c"
CMAKE = ROOT / "CMakeLists.txt"


class Host3dDependencyAudit(unittest.TestCase):
    def test_includes_are_standalone(self):
        allowed = {
            HEADER: {"xg_host_3d_types.h"},
            SOURCE: {
                "xg_host_3d.h",
                "limits.h",
                "stddef.h",
                "string.h",
            },
        }
        include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)

        for path, expected in allowed.items():
            includes = set(include_pattern.findall(path.read_text(encoding="utf-8")))
            self.assertEqual(includes, expected, path)

    def test_target_has_no_link_or_runtime_dependency(self):
        cmake = CMAKE.read_text(encoding="utf-8")
        self.assertIsNone(
            re.search(r"target_link_libraries\s*\(\s*xg_host_3d(?:\s|\))", cmake)
        )

        target_start = cmake.index("add_library(xg_host_3d")
        target_end = cmake.index("add_library(", target_start + 1)
        target_definition = cmake[target_start:target_end]
        self.assertNotIn("psxrecomp", target_definition)
        self.assertNotIn("runtime/include", target_definition)

    def test_forbidden_headers_and_wrappers_are_absent(self):
        combined = HEADER.read_text(encoding="utf-8") + SOURCE.read_text(
            encoding="utf-8"
        )
        for forbidden in ("gte.h", "cpu_state.h", "libgte.h"):
            self.assertNotIn(forbidden, combined)
        for wrapper in (
            r"\bgte_[A-Za-z0-9_]*\s*\(",
            r"\bSet(?:Rot|Trans)Matrix\s*\(",
            r"\bScaleMatrix\s*\(",
        ):
            self.assertIsNone(re.search(wrapper, combined))


if __name__ == "__main__":
    unittest.main()
