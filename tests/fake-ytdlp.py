#!/usr/bin/env python3
"""Replay a captured yt-dlp log at 10 lines per second.

Drop-in replacement for yt-dlp in the elfeed2 config. Ignores all
arguments and finds yt-dlp-log next to this script. Since elfeed2
passes --newline, all output uses \n.

    ytdlp-program = /path/to/fake-ytdlp.py
"""

import os
import re
import time

log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yt-dlp-log")

with open(log_path) as f:
    raw = f.read()

# The captured log has \r-separated progress updates concatenated on
# single lines.  Split them apart into individual output lines.
lines = []
for line in raw.split("\n"):
    if not line:
        continue
    parts = re.split(r"(?=\[download\])", line)
    for part in parts:
        part = part.strip()
        if part:
            lines.append(part)

for line in lines:
    print(line, flush=True)
    time.sleep(0.1)
