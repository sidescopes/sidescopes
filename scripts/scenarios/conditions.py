"""The conditions a measurement was taken under.

Two of these have invalidated measurements on this project before, so they are
recorded with every run rather than remembered: the POWER STATE, because the
reference laptop's numbers only compare on mains and a Mac low on battery
throttles, and the DISPLAY LAYOUT, because the region's size follows the display
it is drawn on and a machine's layout changes between sessions.

Everything here is descriptive. Nothing decides anything; the comparison tool
and the reader do.
"""

import datetime
import hashlib
import plistlib
import re
import subprocess

from . import quartz


def _command(arguments):
    try:
        finished = subprocess.run(arguments, capture_output=True, text=True, timeout=20, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""

    return finished.stdout.strip() if finished.returncode == 0 else ""


def _sysctl(name):
    return _command(["sysctl", "-n", name])


def power_state():
    """Where the machine's power comes from, and how much of it is left.

    The battery percentage matters even on mains: a machine that has just been
    plugged in is charging, which is itself load.
    """
    report = _command(["pmset", "-g", "batt"])
    source = re.search(r"Now drawing from '([^']+)'", report)
    percent = re.search(r"(\d+)%", report)
    thermal = re.search(r"CPU_Speed_Limit\s*=\s*(\d+)", _command(["pmset", "-g", "therm"]))

    return {
        "source": source.group(1) if source else "unknown",
        "battery_percent": int(percent.group(1)) if percent else None,
        "charging": "AC attached; charging" in report,
        "cpu_speed_limit": int(thermal.group(1)) if thermal else None,
    }


def machine():
    """What the measurement ran on."""
    memory = _sysctl("hw.memsize")

    return {
        "name": _command(["hostname", "-s"]),
        "model": _sysctl("hw.model"),
        "cpu": _sysctl("machdep.cpu.brand_string"),
        "logical_cores": int(_sysctl("hw.ncpu") or 0),
        "memory_gb": round(int(memory) / (1024 ** 3), 1) if memory.isdigit() else None,
        "os": f"macOS {_command(['sw_vers', '-productVersion'])} ({_sysctl('kern.osrelease')})",
    }


def application(bundle_path):
    """What was measured: the bundle, its version, and the binary's identity.

    The version string only moves at a release, so two builds either side of a
    development cycle report the same one. The executable's digest is what
    actually tells two runs apart, and what a later reader needs to know that a
    comparison used the binaries it claims.
    """
    executable = bundle_path / "Contents" / "MacOS" / "SideScopes"
    version = ""
    try:
        with open(bundle_path / "Contents" / "Info.plist", "rb") as handle:
            version = plistlib.load(handle).get("CFBundleShortVersionString", "")
    except (OSError, plistlib.InvalidFileException):
        pass
    digest = ""
    try:
        with open(executable, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()[:12]
    except OSError:
        pass

    return {
        "bundle": str(bundle_path),
        "version": version,
        "binary_sha256": digest,
    }


def collect(bundle_path, target_display, content_set):
    """Everything about a run that is not a measurement."""
    return {
        "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "machine": machine(),
        "power": power_state(),
        "displays": quartz.displays(),
        "target_display": target_display,
        "content": content_set,
        "application": application(bundle_path),
    }
