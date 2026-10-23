#!/usr/bin/env python3
"""Reject tracked files with whitespace, newline, or size hygiene violations."""

from pathlib import Path
import subprocess
import sys


def main() -> int:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    )
    paths = [Path(value.decode()) for value in result.stdout.split(b"\0") if value]
    failures = []
    max_bytes = 1024 * 1024

    for path in paths:
        content = path.read_bytes()
        if len(content) > max_bytes:
            failures.append(f"{path}: exceeds 1 MiB")
        if b"\0" in content:
            continue
        if content and not content.endswith(b"\n"):
            failures.append(f"{path}: missing final newline")
        for line_number, line in enumerate(content.splitlines(), start=1):
            if line.endswith((b" ", b"\t")):
                failures.append(f"{path}:{line_number}: trailing whitespace")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"Repository hygiene passed for {len(paths)} tracked files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
