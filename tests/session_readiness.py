"""Exercise native session readiness without contacting the host user manager."""

import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


HELPER = r'''#!PYTHON
import json
import os
from pathlib import Path
import socket
import sys

root = Path(os.environ["READINESS_ROOT"])
def record(event):
    with (root / "events").open("a") as stream:
        stream.write(event + "\n")

name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "systemctl":
    assert args[:1] == ["--user"], args
    if args[1:] == ["show-environment"]:
        sys.exit(0)
    assert "NOTIFY_SOCKET" not in os.environ
    assert os.environ["WAYLAND_DISPLAY"] == "wayland-0"
    assert os.environ["XDG_CURRENT_DESKTOP"] == "umbriel"
    if args[1] == "import-environment":
        record("systemd-imported")
    elif args[1:] == ["set-environment", "UMBRIEL_READINESS_TEST=configured"]:
        record("configured-imported")
    elif args[1:] == ["start", "--no-block", "umbriel-session.target"] and os.environ["READINESS_MANAGED"] == "false":
        record("target")
    else:
        raise AssertionError("unexpected manager command: " + repr(args))
elif name == "dbus-update-activation-environment":
    assert "NOTIFY_SOCKET" not in os.environ
    assert "UMBRIEL_READINESS_TEST" not in os.environ
    record("dbus-waiting")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as gate:
        gate.connect(str(root / "import-gate"))
        gate.sendall(b"waiting")
        if gate.recv(16) != b"release":
            sys.exit(1)
    record("dbus-imported")
elif name == "systemd-run":
    raise AssertionError("readiness must not launch a managed application before reporting ready")
elif name in ("startup", "autostart"):
    assert os.environ["UMBRIEL_READINESS_TEST"] == "configured"
    assert "NOTIFY_SOCKET" not in os.environ
    assert "dbus-imported" in (root / "events").read_text().splitlines(), "autostart preceded environment import"
    record(name)
else:
    raise AssertionError(name)
'''


def events(root):
    path = root / "events"
    return path.read_text().splitlines() if path.exists() else []


def expect_no_readiness(notify, message):
    notify.settimeout(0.25)
    try:
        packet = notify.recv(128)
    except socket.timeout:
        return
    raise AssertionError(f"{message}: {packet!r}")


def check(binary, abstract=False, fail_import=False, managed=True):
    with tempfile.TemporaryDirectory(prefix="um-ready.") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        helper = root / "helper"
        helper.write_text(HELPER.replace("#!PYTHON", "#!" + sys.executable, 1))
        helper.chmod(0o700)
        for name in ("systemctl", "dbus-update-activation-environment", "systemd-run", "startup", "autostart"):
            (root / "bin" / name).symlink_to(helper)
        config = root / "config.toml"
        config.write_text(
            '[general]\nxwayland = false\nshow_cheatsheet = false\n'
            f'autostart = {json.dumps([] if managed else [str(root / "bin" / "autostart")])}\n'
            '[environment]\nUMBRIEL_READINESS_TEST = "configured"\n'
        )

        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notify, \
                socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as gate:
            notify.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            notify_name = str(root / "notify")
            if abstract:
                notify_name = "@" + root.name
            notify.bind("\0" + notify_name[1:] if abstract else notify_name)
            gate.bind(str(root / "import-gate"))
            gate.listen(1)
            gate.settimeout(10)
            environment = os.environ.copy()
            for name in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "UMBRIEL_SOCKET",
                         "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
                         "UMBRIEL_READINESS_TEST", "SYSTEMD_EXEC_PID"):
                environment.pop(name, None)
            environment.update(
                PATH=str(root / "bin") + ":" + os.environ["PATH"],
                XDG_RUNTIME_DIR=str(root),
                XDG_CACHE_HOME=str(root / "cache"),
                DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
                SYSTEMD_BUS_ADDRESS="unix:path=" + str(root / "no-manager-bus"),
                DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
                NOTIFY_SOCKET=notify_name,
                READINESS_ROOT=str(root),
                READINESS_MANAGED="true" if managed else "false",
                WLR_BACKENDS="headless",
                WLR_HEADLESS_OUTPUTS="1",
                WLR_LIBINPUT_NO_DEVICES="1",
            )
            with (root / "compositor.log").open("w+") as log:
                command = [str(binary), "-c", str(config)]
                if managed:
                    command = ["/bin/sh", "-c", 'export SYSTEMD_EXEC_PID=$$; exec "$@"',
                               "session-readiness", *command]
                else:
                    command += ["-s", str(root / "bin" / "startup")]
                process = subprocess.Popen(
                    command,
                    env=environment, stdout=log, stderr=log, start_new_session=True,
                )
                try:
                    connection, _ = gate.accept()
                    with connection:
                        connection.settimeout(5)
                        assert connection.recv(16) == b"waiting"
                        assert events(root) == ["systemd-imported", "configured-imported", "dbus-waiting"]
                        expect_no_readiness(notify, "readiness preceded environment import")
                        assert "startup" not in events(root) and "autostart" not in events(root)
                        connection.sendall(b"fail" if fail_import else b"release")
                    if fail_import:
                        assert process.wait(timeout=10) != 0, "failed environment import accepted"
                        expect_no_readiness(notify, "readiness reported after failed environment import")
                        assert "autostart" not in events(root)
                    elif managed:
                        notify.settimeout(10)
                        packet, credentials, _, _ = notify.recvmsg(128, socket.CMSG_SPACE(12))
                        assert packet == b"READY=1", packet
                        sender = next(struct.unpack("3i", data)[0] for level, kind, data in credentials
                                      if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS)
                        assert sender == process.pid, "readiness must come from the compositor"
                        assert events(root)[-1] == "dbus-imported"
                    else:
                        deadline = time.monotonic() + 5
                        while not {"startup", "autostart"}.issubset(events(root)):
                            assert process.poll() is None, "compositor exited before autostart"
                            assert time.monotonic() < deadline, "autostart did not run after readiness"
                            time.sleep(0.01)
                        assert events(root)[-3] == "target"
                        assert sorted(events(root)[-2:]) == ["autostart", "startup"]
                        expect_no_readiness(notify, "unmanaged compositor used an inherited notification socket")
                except BaseException:
                    log.seek(0)
                    print(log.read(), file=sys.stderr)
                    raise
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
                        if not fail_import:
                            assert process.returncode == 0, "private compositor did not shut down cleanly"


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    check(binary)
    check(binary, abstract=True)
    check(binary, fail_import=True)
    check(binary, managed=False)
    print("environment import gates readiness and autostart; failed imports never report ready")
