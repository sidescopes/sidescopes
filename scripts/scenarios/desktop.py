"""The platform layer, chosen by the system the harness is running on.

Everything the harness needs from an operating system lives behind these names -
the display layout, the pointer and keyboard events, the window list and the
per-process counters - and every module above this one is written against them
rather than against a system. quartz.py and x11.py each implement the whole set;
this decides which is in front of them.

The two are not interchangeable in what they MEAN, only in what they are called.
Their own docstrings say where a number from one does not compare with a number
from the other - coordinates in points against coordinates in pixels, and a
macOS phys_footprint against what Linux charges a process - and a results file
records the system it was taken on so that a reader is never left to guess.
"""

import sys

if sys.platform == "darwin":
    from .quartz import *  # noqa: F401,F403 - this module IS the re-export
elif sys.platform.startswith("linux"):
    from .x11 import *  # noqa: F401,F403 - this module IS the re-export
else:
    raise ImportError(f"the scenario harness has no platform layer for {sys.platform}")
