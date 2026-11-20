#!/usr/bin/env python3
"""Write a NOR image holding one saved-settings record (2.2.3+ layout,
ver 6) for boot_nor.py.

usage: mknor.py out.bin [key=value ...] [--fuzz SEED] [--raw-tail]

keys are the struct persist / struct daily field names below. --fuzz
fills every field (and the padding) with random bytes, then applies the
key=value pairs, and still gives the record a good CRC: the firmware has
to cope with whatever an older or broken build left behind.
"""
import random, struct, sys

FIELDS = [  # name, struct code (persist, ver 6, 2.4.1 layout)
    ('magic', 'I'), ('ver', 'B'), ('brightness', 'B'), ('dnd', 'B'),
    ('airplane', 'B'), ('water_lock', 'B'), ('bt_on', 'B'), ('cc_hide', 'B'),
    ('water_q', 'B'), ('alarm_on', 'B'), ('goal_water_q', 'B'),
    ('goal_steps', 'H'), ('steps_total', 'I'), ('chip_at_save', 'I'),
    ('alarm_h', 'B'), ('alarm_m', 'B'), ('last_sleep_min', 'H'),
    ('last_workout_min', 'H'), ('last_workout_sets', 'B'), ('s_track', 'B'),
    ('clock_tod_ms', 'I'), ('clock_year', 'H'), ('clock_mon', 'B'),
    ('clock_day', 'B'), ('clock_dow', 'B'), ('clock_ok', 'B'),
    ('face_dig', 'B'), ('hr_on', 'B'), ('compat', 'B'), ('pad41', 'B'),
    ('alarm_rang_day', 'H'),
    # struct daily dl
    ('day', 'I'), ('steps_base', 'I'), ('water_ml', 'H'), ('rest_bpm', 'B'),
    ('goals_hit', 'B'), ('y_day', 'I'), ('y_steps', 'I'), ('y_water', 'H'),
    ('y_rest', 'B'), ('y_pad', 'B'),
    ('sleep_day', 'I'),
]
FMT = '<' + ''.join(c for _, c in FIELDS)
assert struct.calcsize(FMT) == 76, struct.calcsize(FMT)

DEFAULTS = dict(
    magic=0x564F4944, ver=6, brightness=2, bt_on=1, alarm_on=1,
    steps_total=50000, chip_at_save=0, alarm_h=7, alarm_m=0,
    clock_tod_ms=15 * 3600000, clock_year=2026, clock_mon=10, clock_day=31,
    clock_dow=6, clock_ok=1, face_dig=1, hr_on=1, compat=1,
    day=20261031, steps_base=38000, water_ml=2250, rest_bpm=58,
    y_day=20261030, y_steps=11000, y_water=1500, y_rest=57,
    sleep_day=20261031)

def crc(data, seq):
    c = (~seq) & 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1) & 0xFFFFFFFF))
    return (~c) & 0xFFFFFFFF

def record(vals, rnd=None):
    if rnd is None:
        body = struct.pack(FMT, *[vals.get(n, 0) for n, _ in FIELDS])
    else:
        body = bytearray(rnd.getrandbits(8) for _ in range(76))
        off = 0
        for n, c in FIELDS:
            sz = struct.calcsize('<' + c)
            if n in vals:
                body[off:off + sz] = struct.pack('<' + c, vals[n])
            off += sz
        body = bytes(body)
    seq = 7
    return body + struct.pack('<III', 0x56544C32, seq, crc(body, seq))

if __name__ == '__main__':
    out = sys.argv[1]
    vals, rnd = {}, None
    a = sys.argv[2:]
    i = 0
    while i < len(a):
        if a[i] == '--fuzz':
            rnd = random.Random(int(a[i + 1]))
            i += 2
            continue
        k, v = a[i].split('=')
        vals[k] = int(v, 0)
        i += 1
    if rnd is None:
        vals = {**DEFAULTS, **vals}
    else:
        vals = {'magic': 0x564F4944, 'ver': 6, **vals}
    nor = bytearray(b'\xff' * (4 * 1024 * 1024))
    rec = record(vals, rnd)
    nor[0x3F0000:0x3F0000 + len(rec)] = rec
    open(out, 'wb').write(nor)
