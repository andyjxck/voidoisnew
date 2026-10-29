"""The Google Material Symbols VoidOS draws, and the pixel sizes each one is
baked at. Shared by tools/render_micons.py (font -> assets/micons/*.png)
and tools/gen_assets.py (PNGs -> gen/micons.c + gen/micons.h).

Each row: (C id suffix, Material Symbols name, FILL axis, sizes in px).
Only the sizes listed here end up in flash (A8, px*px bytes each).
"""
import os

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "assets",
                       "micons")

ICONS = [
    # launcher apps: satellites 20, hub 36
    ("FITNESS",       "fitness_center",        1, [20, 24, 36]),
    ("STATS",         "bar_chart",             1, [20, 24, 36]),
    ("FAVORITE",      "favorite",              1, [16, 20, 24, 36]),
    ("BEDTIME",       "bedtime",               1, [20, 24, 36]),
    ("WATER_DROP",    "water_drop",            1, [16, 20, 24, 36]),
    ("ALARM",         "alarm",                 1, [20, 24, 36]),
    ("NOTIFICATIONS", "notifications",         1, [20, 24, 36]),
    ("SETTINGS",      "settings",              1, [20, 24, 36]),
    # control centre and stats wheel: satellites 20, hub 24
    ("MONITOR_HEART", "monitor_heart",         1, [20, 24]),
    ("SMARTPHONE",    "smartphone",            1, [20, 24]),
    ("LIGHT_MODE",    "light_mode",            1, [20, 24]),
    ("BLUETOOTH",     "bluetooth",             1, [20, 24]),
    ("FLIGHT",        "flight",                1, [20, 24]),
    ("FLASHLIGHT",    "flashlight_on",         1, [20, 24]),
    ("DND",           "do_not_disturb_on",     1, [20, 24]),
    ("WATER_LOCK",    "water_lock",            1, [20, 24]),
    ("FOOTPRINT",     "footprint",             1, [16, 20, 24]),
    ("FIRE",          "local_fire_department", 1, [20, 24]),
    ("BOLT",          "bolt",                  1, [16, 20, 24, 36]),
    ("BATT_0",        "battery_0_bar",         1, [20, 24]),
    ("BATT_1",        "battery_1_bar",         1, [20, 24]),
    ("BATT_2",        "battery_2_bar",         1, [20, 24]),
    ("BATT_3",        "battery_3_bar",         1, [20, 24]),
    ("BATT_4",        "battery_4_bar",         1, [20, 24]),
    ("BATT_5",        "battery_5_bar",         1, [20, 24]),
    ("BATT_6",        "battery_6_bar",         1, [20, 24]),
    ("BATT_FULL",     "battery_full",          1, [20, 24]),
    ("BATT_CHARGING", "battery_charging_full", 1, [16, 20, 24]),
    ("BATT_ALERT",    "battery_alert",         1, [20, 24]),
    # music
    ("MUSIC_NOTE",    "music_note",            1, [16, 20, 24]),
    ("PLAY",          "play_arrow",            1, [16, 20, 28]),
    ("PAUSE",         "pause",                 1, [16, 20, 28]),
    ("SKIP_NEXT",     "skip_next",             1, [24]),
    ("SKIP_PREV",     "skip_previous",         1, [24]),
    ("VOLUME_UP",     "volume_up",             1, [20]),
    ("VOLUME_DOWN",   "volume_down",           1, [20]),
    # buttons and empty states
    ("CHEVRON_LEFT",  "chevron_left",          1, [24]),
    ("CHEVRON_RIGHT", "chevron_right",         1, [20]),
    ("ADD",           "add",                   1, [24]),
    ("REMOVE",        "remove",                1, [24]),
    ("INBOX",         "inbox",                 1, [24]),
]
