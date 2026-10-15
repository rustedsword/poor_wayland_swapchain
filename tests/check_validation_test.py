import ctypes
import os
import select
import signal
import subprocess
import sys
from pathlib import Path
import time

checker = Path(__file__).with_name("check-validation.py")
cases = [
    ("print('clean')", 0),
    ("raise SystemExit(42)", 42),
    ("raise SystemExit(77)", 77),
    ("print('Validation Error: example')", 1),
    ("import sys; print('VUID-example', file=sys.stderr)", 1),
    ("print('SYNC-HAZARD-example'); raise SystemExit(77)", 1),
    ("import atexit; atexit.register(print, 'Validation Error: shutdown')", 1),
    ("print('ERROR: LeakSanitizer: detected memory leaks')", 1),
    ("import subprocess, sys; subprocess.run([sys.executable, '-c', \"print('Validation Error: child')\"])", 1),
    ("import subprocess, sys; subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])", 1),
]
for code, expected in cases:
    result = subprocess.run([sys.executable, str(checker), sys.executable, "-c", code],
                            capture_output=True, timeout=10)
    assert result.returncode == expected, (code, result.returncode, result.stdout, result.stderr)

# Keep orphaned helpers as our children until explicitly reaped, as happens
# under a container init or subreaper. Their exit must not fail a clean test.
libc = ctypes.CDLL(None, use_errno=True)
assert libc.prctl(36, 1, 0, 0, 0) == 0, ctypes.get_errno()  # PR_SET_CHILD_SUBREAPER
code = """
import os
pid = os.fork()
if pid == 0:
    os._exit(0)
print(pid, flush=True)
"""
result = subprocess.run([sys.executable, str(checker), sys.executable, "-c", code],
                        capture_output=True, timeout=10)
helper = int(result.stdout)
assert os.waitpid(helper, 0) == (helper, 0)
assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)


def check_timeout(exit_parent):
    # Both the test and its helper resist SIGTERM. Check interruption while
    # waiting for the test and while giving its orphaned helper time to exit.
    code = """
import os, signal, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
pid = os.fork()
if pid:
    print(os.getpid(), pid, flush=True)
    if sys.argv[1] == 'exit':
        os._exit(0)
time.sleep(60)
"""
    process = subprocess.Popen(
        [sys.executable, str(checker), sys.executable, "-c", code,
         "exit" if exit_parent else "wait"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    group = None
    try:
        assert select.select([process.stdout], [], [], 5)[0], "Test did not start"
        group, helper = map(int, process.stdout.readline().split())
        if exit_parent:
            deadline = time.monotonic() + 5
            while Path(f"/proc/{group}").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert not Path(f"/proc/{group}").exists(), "Test did not exit"

        # Match Meson's timeout policy, including its separate process group.
        os.killpg(process.pid, signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(timeout=0.5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 128 + signal.SIGTERM, (exit_parent, process.returncode, stdout, stderr)
        assert not Path(f"/proc/{group}").exists(), "Test survived timeout"
        pid, status = os.waitpid(helper, os.WNOHANG)
        assert pid == helper, "Helper survived timeout"
        assert os.waitstatus_to_exitcode(status) == -signal.SIGKILL
    finally:
        # Also clean up if the assertions fail against a broken checker.
        for pid in (process.pid, group):
            if pid is not None:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        process.communicate(timeout=5)
        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                break


check_timeout(exit_parent=False)
check_timeout(exit_parent=True)
print("Validation checker: diagnostics, orphan cleanup, exited helpers and timeout signals checked")
