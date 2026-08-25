from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

from run_with_job_pool import _unlock, acquire_slot


def test_command_waits_until_a_pool_slot_is_available(tmp_path: Path) -> None:
    lock_dir = tmp_path / "pool"
    output = tmp_path / "ran"
    held_slot = acquire_slot(lock_dir, 1)
    runner = Path(__file__).with_name("run_with_job_pool.py")
    process = subprocess.Popen([
        sys.executable,
        str(runner),
        "--lock-dir",
        str(lock_dir),
        "--jobs",
        "1",
        "--",
        sys.executable,
        "-c",
        "from pathlib import Path; import sys; Path(sys.argv[1]).touch()",
        str(output),
    ])
    try:
        time.sleep(0.15)
        assert process.poll() is None
        assert not output.exists()
    finally:
        _unlock(held_slot)
        held_slot.close()

    assert process.wait(timeout=5) == 0
    assert output.is_file()
