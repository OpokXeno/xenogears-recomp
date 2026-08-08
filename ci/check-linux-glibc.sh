#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:?usage: check-linux-glibc.sh <binary> [max-glibc]}"
MAX_GLIBC="${2:-GLIBC_2.31}"

python3 - "$BINARY" "$MAX_GLIBC" <<'PY'
import re
import subprocess
import sys

binary = sys.argv[1]
maximum = tuple(int(part) for part in sys.argv[2].removeprefix("GLIBC_").split("."))
output = subprocess.check_output(
    ["readelf", "--version-info", binary], text=True, stderr=subprocess.STDOUT
)
versions = sorted({
    tuple(int(part) for part in value.split("."))
    for value in re.findall(r"GLIBC_(\d+(?:\.\d+)+)", output)
})
if not versions:
    raise SystemExit(f"No GLIBC symbol versions found in {binary}")

latest = versions[-1]
def display(version: tuple[int, ...]) -> str:
    return "GLIBC_" + ".".join(str(part) for part in version)

print(f"{binary}: maximum required {display(latest)}; allowed {display(maximum)}")
if latest > maximum:
    raise SystemExit(
        f"{binary} requires {display(latest)}, newer than allowed {display(maximum)}"
    )
PY
