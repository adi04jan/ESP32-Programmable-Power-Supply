import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))

from voltbench import connect                       # noqa: E402
from voltbench.transport import find_sim_binary     # noqa: E402


def _ensure_sim() -> Path:
    """Build tools/sim/vb_sim if it is missing or older than the firmware headers.

    The staleness check matters: the whole value of this simulator is that it
    IS the firmware logic, so a stale binary silently turns these into tests of
    last week's code.
    """
    binary = find_sim_binary()
    src = ROOT / "tools" / "sim" / "vb_sim.cpp"
    headers = sorted((ROOT / "Voltbench_v2").glob("vb_*.h"))
    newest_src = max([src] + headers, key=lambda p: p.stat().st_mtime)

    if binary and binary.exists() and binary.stat().st_mtime >= newest_src.stat().st_mtime:
        return binary

    out = src.with_suffix(".exe" if sys.platform == "win32" else "")
    r = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-O2", "-o", str(out), str(src)],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        pytest.exit(f"cannot build vb_sim:\n{r.stderr}", returncode=1)
    return out


@pytest.fixture(scope="session")
def sim_binary():
    return _ensure_sim()


@pytest.fixture
def psu(sim_binary):
    """A fresh simulated supply per test — no state bleeds between tests."""
    p = connect(f"sim://{sim_binary}")
    yield p
    p.t.close()
