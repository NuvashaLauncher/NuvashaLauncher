#!/usr/bin/env python3
"""Parallel isolated test runner for NuvashaLauncher."""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile


def run_one(idx: int, binary: str, root_tmp: str, schema_dir: str):
    work = os.path.join(root_tmp, f"t-{idx}")
    for sub in ("config", "data", "cache", "state", "runtime"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    os.chmod(os.path.join(work, "runtime"), 0o700)

    env = os.environ.copy()
    env.update(
        {
            "NUVASHALAUNCHER_TEST_SECRET_STORE": "1",
            "NUVASHALAUNCHER_TEST_SECRET_STORE_FILE": os.path.join(work, "secrets.json"),
            "NUVASHALAUNCHER_TEST_FAST_TIMERS": "1",
            "TMPDIR": work,
            "XDG_CONFIG_HOME": os.path.join(work, "config"),
            "XDG_DATA_HOME": os.path.join(work, "data"),
            "XDG_CACHE_HOME": os.path.join(work, "cache"),
            "XDG_STATE_HOME": os.path.join(work, "state"),
            "XDG_RUNTIME_DIR": os.path.join(work, "runtime"),
            "GSETTINGS_SCHEMA_DIR": schema_dir,
            "GSETTINGS_BACKEND": "memory",
        }
    )

    proc = subprocess.run(
        [f"./{binary}"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    return idx, binary, proc.returncode, proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("binaries", nargs="+")
    args = parser.parse_args()

    os.makedirs("build", exist_ok=True)
    root_tmp = tempfile.mkdtemp(prefix="test-tmp-", dir="build")
    passed = 0
    skipped = 0

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = [
                pool.submit(run_one, i, b, root_tmp, args.schema_dir)
                for i, b in enumerate(args.binaries)
            ]
            results = [f.result() for f in futures]

        for _, binary, status, output in sorted(results, key=lambda r: r[0]):
            sys.stdout.write(f"RUN  {binary}\n{output}")
            if status == 77:
                skipped += 1
            elif status != 0:
                sys.stdout.flush()
                return status
            elif re.search(r"^TAP version", output, re.MULTILINE):
                tap_skip = len(re.findall(r"^ok .*# SKIP", output, re.MULTILINE))
                tap_pass = len(re.findall(r"^ok ", output, re.MULTILINE))
                passed += tap_pass - tap_skip
                skipped += tap_skip
            else:
                passed += 1

        print(f"PASS {passed} test(s), SKIP {skipped} test(s)")
        return 0
    finally:
        shutil.rmtree(root_tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
