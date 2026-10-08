#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Writes a synthetic click track. Nothing here is from a commercial disc.

import math
import struct
import sys

path = sys.argv[1]
rate = 44100
seconds = 22
frames = rate * seconds
samples = bytearray()
for i in range(frames):
    tick = i % (rate // 2)
    if tick < 180:
        env = 1.0 - tick / 180.0
        tone = math.sin(2.0 * math.pi * (90 if (i // (rate // 2)) % 2 == 0 else 3200) * i / rate)
        value = int(max(-32767, min(32767, tone * env * 22000)))
    else:
        value = 0
    samples += struct.pack("<hh", value, value)

data = bytes(samples)
header = struct.pack(
    "<4sI4s4sIHHIIHH4sI",
    b"RIFF",
    36 + len(data),
    b"WAVE",
    b"fmt ",
    16,
    1,
    2,
    rate,
    rate * 4,
    4,
    16,
    b"data",
    len(data),
)
with open(path, "wb") as out:
    out.write(header)
    out.write(data)
