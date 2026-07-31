"""The conditions a measurement was taken under.

Two of these have invalidated measurements on this project before, so they are
recorded with every run rather than remembered: the POWER STATE, because the
reference laptop's numbers only compare on mains and a Mac low on battery
throttles, and the DISPLAY LAYOUT, because the region's size follows the display
it is drawn on and a machine's layout changes between sessions.

A third joins them on Linux, where one machine can run the application two ways:
the GRAPHICS SESSION, because an X session has the application capturing the
screen itself and a Wayland one has it capturing through the portal. Those are
different pipelines, so two runs that do not name which one they took would be
compared as though they measured the same thing.

Everything here is descriptive. Nothing decides anything; the comparison tool
and the reader do.
"""

import datetime
import hashlib
import os
import pathlib
import platform
import plistlib
import re
import subprocess
import sys

from . import desktop


def _command(arguments):
    try:
        finished = subprocess.run(arguments, capture_output=True, text=True, timeout=20, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""

    return finished.stdout.strip() if finished.returncode == 0 else ""


def _sysctl(name):
    return _command(["sysctl", "-n", name])


def _first_line(path):
    """One line out of a /proc or /sys file, or an empty string if it is not there."""
    try:
        with open(path, "r", errors="replace") as handle:
            return handle.readline().strip()
    except OSError:
        return ""


def _digest_of(path):
    """The first twelve hex digits of a file's SHA-256, or an empty string.

    Short because it is read by a person comparing two runs, and twelve digits
    of SHA-256 do not collide across the handful of builds a comparison holds.
    """
    try:
        with open(path, "rb") as handle:
            return hashlib.sha256(handle.read()).hexdigest()[:12]
    except OSError:
        return ""


def power_state():
    """Where the machine's power comes from, and how much of it is left.

    The battery percentage matters even on mains: a machine that has just been
    plugged in is charging, which is itself load.
    """
    return _macos_power() if sys.platform == "darwin" else _linux_power()


def _macos_power():
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


_POWER_SUPPLIES = pathlib.Path("/sys/class/power_supply")


def _linux_power():
    """Read from the supplies the kernel exposes.

    The vocabulary is deliberately the one the macOS branch uses - "AC Power",
    "Battery Power" - because a results file is read beside one taken on the
    other system, and two words for one state would be read as two states.

    A mains supply that is online decides it. Where the machine has none at all,
    which is what a virtual one looks like, the answer is that it does not know
    rather than a guess at mains.
    """
    supplies = [(path, _first_line(path / "type")) for path in sorted(_POWER_SUPPLIES.glob("*"))]
    mains = [path for path, kind in supplies if kind == "Mains"]
    batteries = [path for path, kind in supplies if kind == "Battery"]
    states = [_first_line(path / "status") for path in batteries]
    charges = [_first_line(path / "capacity") for path in batteries]
    percents = [int(charge) for charge in charges if charge.isdigit()]
    if any(_first_line(path / "online") == "1" for path in mains):
        source = "AC Power"
    else:
        source = "Battery Power" if batteries else "unknown"

    return {
        "source": source,
        "battery_percent": percents[0] if percents else None,
        "charging": "Charging" in states,
        # No counterpart here: the thermal ceiling macOS reports as a percentage
        # of full speed is not a figure this kernel keeps. Left unanswered
        # rather than filled with something that means something else.
        "cpu_speed_limit": None,
    }


def machine():
    """What the measurement ran on.

    The same keys from whatever each system can be asked - sysctl on a Mac,
    /proc and /sys on Linux - plus one the Mac has no question for: which
    graphics session the application is being launched into, because that
    decides how it captures the screen rather than only how fast it does.
    """
    return _macos_machine() if sys.platform == "darwin" else _linux_machine()


def _macos_machine():
    memory = _sysctl("hw.memsize")

    return {
        "name": _command(["hostname", "-s"]),
        "model": _sysctl("hw.model"),
        "cpu": _sysctl("machdep.cpu.brand_string"),
        "logical_cores": int(_sysctl("hw.ncpu") or 0),
        "memory_gb": round(int(memory) / (1024 ** 3), 1) if memory.isdigit() else None,
        "os": f"macOS {_command(['sw_vers', '-productVersion'])} ({_sysctl('kern.osrelease')})",
    }


# Where the firmware's own name for the machine is published. The second path is
# the first one's target rather than a different answer, read only for a system
# whose /sys/class is arranged some other way.
_MODEL_PATHS = ("/sys/class/dmi/id/product_name", "/sys/devices/virtual/dmi/id/product_name")

# The field a processor's name arrives under, by architecture: x86 kernels write
# "model name", and ARM ones name the board under "Hardware" and nothing at all
# under the first two.
_CPU_FIELDS = ("model name", "Model", "Hardware")


def _linux_machine():
    return {
        "name": platform.node().split(".")[0],
        "model": next((found for found in (_first_line(path) for path in _MODEL_PATHS) if found), ""),
        "cpu": _cpu_name(),
        "logical_cores": os.cpu_count() or 0,
        "memory_gb": _memory_gb(),
        "os": f"{_distribution()} ({platform.release()})",
        "session": desktop.session_facts(),
    }


def _cpu_name():
    """The processor's own name for itself, from /proc/cpuinfo."""
    fields = {}
    try:
        with open("/proc/cpuinfo", "r", errors="replace") as handle:
            for line in handle:
                name, separator, value = line.partition(":")
                if separator:
                    fields.setdefault(name.strip(), value.strip())
    except OSError:
        return ""

    return next((fields[name] for name in _CPU_FIELDS if fields.get(name)), "")


def _memory_gb():
    """What the kernel has to hand out, which is a little under what is fitted.

    MemTotal rather than the firmware's figure: the difference is memory the
    firmware kept, and the part the kernel manages is the part a measurement
    competes for.
    """
    try:
        with open("/proc/meminfo", "r", errors="replace") as handle:
            for line in handle:
                if line.startswith("MemTotal:"):
                    return round(int(line.split()[1]) / (1024 ** 2), 1)
    except (OSError, ValueError, IndexError):
        pass

    return None


def _distribution():
    """PRETTY_NAME from /etc/os-release, which every distribution writes."""
    try:
        with open("/etc/os-release", "r", errors="replace") as handle:
            for line in handle:
                if line.startswith("PRETTY_NAME="):
                    return line.partition("=")[2].strip().strip('"')
    except OSError:
        pass

    return "Linux"


def application(application_path, executable):
    """What was measured: where it is, and what identifies the binary.

    The version string only moves at a release, so two builds either side of a
    development cycle report the same one. The executable's digest is what
    actually tells two runs apart, and what a later reader needs to know that a
    comparison used the binaries it claims.
    """
    return (_macos_application(application_path, executable) if sys.platform == "darwin"
            else _linux_application(executable))


def _macos_application(bundle_path, executable):
    version = ""
    try:
        with open(bundle_path / "Contents" / "Info.plist", "rb") as handle:
            version = plistlib.load(handle).get("CFBundleShortVersionString", "")
    except (OSError, plistlib.InvalidFileException):
        pass

    return {
        "bundle": str(bundle_path),
        "version": version,
        "binary_sha256": _digest_of(executable),
    }


# A Linux build states its version NOWHERE a script can read it: main() takes no
# arguments (src/app/main.cpp), so there is no --version to ask, and there is no
# bundle beside the binary carrying one. The version compiled into it could be
# fished out of the binary's strings, but a run of text that looks like a version
# is not the same claim as a version the build states - and a label that can lie
# is worse than none. So the field says what it does not know, and the digest,
# the size and the modification time are what identify this build instead.
_UNKNOWN_VERSION = "unknown"
_UNKNOWN_VERSION_REASON = ("a Linux build states no version outside its own window: main() takes no arguments and no "
                           "file beside the binary carries one, so this run identifies the build by its digest")


def _linux_application(executable):
    stat = None
    try:
        stat = executable.stat()
    except OSError:
        pass

    return {
        "executable": str(executable),
        "version": _UNKNOWN_VERSION,
        "version_unknown_because": _UNKNOWN_VERSION_REASON,
        "binary_sha256": _digest_of(executable),
        "built": (datetime.datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(timespec="seconds")
                  if stat else ""),
        "size_bytes": stat.st_size if stat else None,
    }


def collect(application_path, executable, target_display, content_set):
    """Everything about a run that is not a measurement."""
    return {
        "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "machine": machine(),
        "power": power_state(),
        "displays": desktop.displays(),
        "target_display": target_display,
        "content": content_set,
        "application": application(application_path, executable),
    }
