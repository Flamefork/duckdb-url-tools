import platform
from datetime import datetime
from datetime import timezone
from os import cpu_count
from subprocess import run

import duckdb

from config import PROJECT_ROOT


def git_commit() -> str:
    result = run(
        ["git", "rev-parse", "--short", "HEAD"],
        capture_output=True,
        text=True,
        cwd=PROJECT_ROOT,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def collect_environment() -> dict:
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "duckdb": duckdb.__version__,
        "cpu_count": cpu_count(),
        "git_commit": git_commit(),
    }
