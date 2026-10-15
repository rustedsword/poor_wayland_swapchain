#!/usr/bin/env python3
"""Run a test and reject validation or sanitizer diagnostics from its process tree."""

import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time


def validation_environment(log_path, enabled, data_directory):
    # Keep driver/layer search paths, but isolate validation from vkconfig and
    # shell overrides. The test policy must also reach exec'd clients.
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("VK_LAYER_", "VK_VALIDATION_", "VK_KHRONOS_VALIDATION_"))
        or key == "VK_LAYER_PATH"
    }
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment.pop("VK_LOADER_LAYERS_ALLOW", None)
    # The layer reads vkconfig's XDG_DATA_HOME settings before
    # VK_LAYER_SETTINGS_PATH. Use an empty data home, retaining the original
    # directory in XDG_DATA_DIRS so user-installed drivers/layers remain visible
    # in the same search order.
    original_data = environment.get("XDG_DATA_HOME") or str(Path.home() / ".local/share")
    environment["XDG_DATA_DIRS"] = os.pathsep.join([
        original_data, environment.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"])
    environment["XDG_DATA_HOME"] = data_directory
    environment["VK_LOADER_LAYERS_ENABLE"] = "VK_LAYER_KHRONOS_validation" if enabled else ""
    environment["VK_LOADER_LAYERS_DISABLE"] = "" if enabled else "~all~"
    environment["VK_LAYER_SETTINGS_PATH"] = str(Path(__file__).with_name("vk_layer_settings.txt").resolve())
    environment["VK_LAYER_LOG_FILENAME"] = log_path
    return environment


def signal_group(pid, sig):
    try:
        os.killpg(pid, sig)
        return True
    except ProcessLookupError:
        return False


def group_running(pid):
    if not signal_group(pid, 0):
        return False
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_bytes().rsplit(b")", 1)[1].split()
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        # State and process group follow the parenthesized command name.
        # Exited helpers may await reaping by an external init or subreaper.
        if int(fields[2]) == pid and fields[0] not in (b"Z", b"X"):
            return True
    return False


def run(command, probe=None, no_layers=False):
    invalid = False
    read_fd, write_fd = os.pipe()
    # Opening this pipe through the checker's /proc entry survives any stdout/
    # stderr redirection or close_fds in a child. A regular shared log would be
    # truncated every time another Vulkan instance opens it with fopen("w").
    log_path = f"/proc/{os.getpid()}/fd/{write_fd}"
    environment = dict(os.environ)

    def collect():
        nonlocal invalid
        with os.fdopen(read_fd, "rb") as output:
            for line in output:
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                invalid |= bool(re.search(
                    rb"Validation (?:Layer )?Error|VUID-|SYNC-HAZARD|runtime error:|"
                    rb"(?:ERROR|WARNING): (?:Address|Leak|Memory|Thread)Sanitizer", line))

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()

    def execute(arguments):
        with subprocess.Popen(arguments, stdout=write_fd, stderr=write_fd,
                              env=environment, start_new_session=True) as process:
            def interrupted(signum, frame):
                signal_group(process.pid, signum)
                raise SystemExit(128 + signum)

            previous = {sig: signal.signal(sig, interrupted)
                        for sig in (signal.SIGTERM, signal.SIGINT)}
            result = 1
            try:
                result = process.wait()
                # Runtime helpers such as llvm-symbolizer exit on pipe EOF,
                # shortly after the instrumented process itself has exited.
                # Interruptions skip this grace period: Meson escalates to
                # SIGKILL after only 0.5 seconds.
                deadline = time.monotonic() + 1
                while group_running(process.pid) and time.monotonic() < deadline:
                    time.sleep(0.01)
            finally:
                # A second interruption must not bypass the SIGKILL below.
                for sig in previous:
                    signal.signal(sig, signal.SIG_IGN)
                try:
                    # A sanitizer can abort the test before its atexit cleanup.
                    # Do not leave a compositor rendering or holding our pipe open.
                    if group_running(process.pid):
                        print("Test left child processes running; terminating them.", file=sys.stderr)
                        signal_group(process.pid, signal.SIGTERM)
                        time.sleep(0.2)
                        signal_group(process.pid, signal.SIGKILL)
                        if result == 0:
                            result = 1
                finally:
                    for sig, handler in previous.items():
                        signal.signal(sig, handler)
        return result

    try:
        with tempfile.TemporaryDirectory(prefix="poor-validation-") as data_directory:
            if probe or no_layers:
                environment = validation_environment(log_path, not no_layers, data_directory)
            result = execute([probe]) if probe else 0
            if result == 0:
                result = execute(command)
    finally:
        os.close(write_fd)
        reader.join(timeout=5)
    if reader.is_alive():
        print("Test left a process holding its diagnostic pipe open.", file=sys.stderr)
        return 1
    if invalid:
        print("Validation or sanitizer diagnostics fail this test.", file=sys.stderr)
        return 1
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--vulkan", metavar="PROBE", help="require and enable Vulkan validation")
    mode.add_argument("--no-layers", action="store_true", help="run without Vulkan layers")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("a test command is required")
    sys.exit(run(args.command, args.vulkan, args.no_layers))
