"""AT-PRJ-11 and AT-PRJ-12: kill the process after unsaved edits, then recover or discard.

usage: kill_recovery.py <weft-cli> <scratch-dir> (recover|discard)
"""

import shutil
import subprocess
import sys
import time
from pathlib import Path


def run(cli, *args):
    result = subprocess.run([cli, *args], capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        sys.exit(f"weft-cli {' '.join(args)} failed: {result.stderr}")
    return result.stdout


def main():
    cli, scratch, mode = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)

    folder = run(cli, "new", str(scratch), "Kill Test").strip()
    saved = scratch / "saved.json"
    run(cli, "open", folder, "--dump", str(saved))

    pre = scratch / "pre-kill.json"
    proc = subprocess.Popen([cli, "demo-edits", folder, "--dump", str(pre), "--hold"], stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline().strip()
    if line != "READY":
        proc.kill()
        sys.exit(f"demo-edits did not get ready: {line!r}")
    time.sleep(2)  # killed 2 s after the last edit
    proc.kill()
    proc.wait()

    status = run(cli, "open", folder)
    if "recovery: available" not in status:
        sys.exit(f"expected recovery to be offered, got {status!r}")

    after = scratch / "after.json"
    log = Path(folder) / "Recovery" / "changes.log"
    if mode == "recover":
        run(cli, "open", folder, "--recover", "--dump", str(after))
        if after.read_bytes() != pre.read_bytes():
            sys.exit("recovered document differs from the pre-kill state")
        if "recovery: none" not in run(cli, "open", folder):
            sys.exit("recovery still offered after Recover")
    else:
        run(cli, "open", folder, "--discard", "--dump", str(after))
        if after.read_bytes() != saved.read_bytes():
            sys.exit("Discard did not keep the last saved state")
        if log.exists():
            sys.exit("Discard left the recovery log in place")
        archived = list((log.parent / "archive").glob("*.log"))
        if len(archived) != 1:
            sys.exit(f"expected one archived log, found {archived}")
    print(f"{mode}: ok")


if __name__ == "__main__":
    main()
