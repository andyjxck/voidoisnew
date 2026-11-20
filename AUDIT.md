# VoidOS whole-watch audit (for 2.2.3)

Base: branch `voidos-2.2.2` plus the 2.2.3 work in progress. Method: reading the
firmware source (ui_nav.c, ui_face.c, hal_sys.c, sensors.c, spi_flash.c,
ble_app.c, ancs.c, main.c), the host tests, and the uisnap screen renderer.
Nothing here was measured on a real watch. Each finding is ranked by how much it
hurts day-to-day use. Status column: FIXED (in 2.2.3), PARTLY, OPEN.

## Ranked findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 1 | Battery % | One raw 12-bit SAADC sample per read, no oversampling, no filter, read while the screen/backlight/motor/HR LED load the cell, so the number jumps (26 > 30 > 25 > 29). Each consumer (control centre every second, diag, BLE battery read) took its own fresh sample, so the watch and the phone disagreed. | FIXED |
| 2 | Battery drain | CPU never slept: `hal_delay_ms` spun on the RTC counter at 64 MHz around the clock. | FIXED (WFE sleep, in the 2.2.3 WIP) |
| 3 | Battery drain | Core ran on the LDO, not the DC/DC converter the PineTime has. | FIXED (DCDCEN) |
| 4 | Battery drain | HR LED on 24/7 (screen off, on the charger, off the wrist). | FIXED (live only with screen on or workout; background reading every 5 min, none on charger) |
| 5 | Battery drain | Advertising at NimBLE's 30-60 ms default forever; iOS link at 30 ms, no latency. | FIXED (fast 30 s then 1022.5 ms; 45-60 ms latency 3 after 10 s) |
| 6 | Battery drain | Screen-off main loop still woke ~40 times a second (20 ms + 5 ms waits), each pass polling I2C. | FIXED (idles 90 ms when HR is off, the side button wakes it at once) |
| 7 | Battery drain | SPI NOR flash woken at boot/settings save and never put back into deep power-down (0xB9). | OPEN (needs a check that the recovery loader wakes the chip with 0xAB before it can be done safely) |
| 8 | Battery drain | Accel sampled at 50 Hz and the Bosch step counter read 50 times a second with the screen off; wrist-raise register read every loop. | FIXED (10 Hz asleep, counter 1 Hz, raise 10 Hz) |
| 9 | Battery drain | Per-screen updaters (second hand, heart, music clock, cardio trace) ran while the panel slept. | FIXED |
| 10 | Battery drain | ANCS retry storm: full GATT discovery every 60 s forever while iOS refused notification access. | FIXED (every 5 min after ~20 min) |
| 11 | Dead/limited screens | CLOCK: stopwatch only start/reset, no laps; timer only 1/5/10 min presets with no custom time; alarm settable only in 15-minute steps, no snooze, rings forever if you are not there, never rings until the clock has been set. Feels like it does nothing useful. | FIXED |
| 12 | Reliability | Settings live in one flash sector with no checksum: erase-then-write, so a reset or flat battery mid-save loses every setting and the step total. | FIXED (two sectors, sequence number + CRC) |
| 13 | Reliability | Settings save does a 4 KB sector erase on the UI thread (up to ~300 ms freeze); deferred 2 s after a change, so it lands while you are still tapping. | OPEN (deferred, not moved off the UI thread) |
| 14 | UX | Side button single press waited for nothing; adding the double press means a single press on an awake screen now waits 300 ms. | ACCEPTED (asleep presses act at once) |
| 15 | Feature | Double press of the side button toggles the HR sensor. | FIXED |
| 16 | Accuracy | Sleep is a manual bedtime timer only (no movement or HR); the label says "LAST NIGHT" even if it was a nap. | OPEN |
| 17 | Accuracy | Steps: Bosch engine counter when it is running, software fallback otherwise; never compared on a real walk. Resting HR is the lowest reading of the day, which one bad low reading can poison. | OPEN |
| 18 | UX | WORKOUT: PUSH/PULL/LEG DAY are just names on the same set counter (no exercises, no reps, fixed max sets). | OPEN (partly working) |
| 19 | UX | SET TIME sets only the time; with no phone sync the date stays 1 Jan 2025. | OPEN |
| 20 | Icons | Every icon is a hand-drawn line glyph (list below). | OPEN |
| 21 | Code health | uisnap, test_hr and LVGL build with `-w` (warnings hidden). BLE parsers bound-check lengths (ANCS data source, control point queue, notification source checked). | OPEN (warnings), OK (bounds checked so far) |
| 22 | Perf | LVGL refresh period 33 ms plus a 5 ms loop delay: touch-to-draw up to ~40 ms; panel wake does SLPOUT with a 120 ms wait. | OPEN |

## Screens (launcher and system)

| Screen | State | What is missing |
|--------|-------|-----------------|
| Watch face | Working | - |
| Launcher | Working | - |
| Control centre | Working | - |
| Workout (pick, run, summary) | Partly working | routines have no content (see 18) |
| Stats | Working | STEPS and ENERGY tiles open nothing |
| Cardio | Working | - |
| Sleep (+ night summary) | Partly working | manual only (see 16); WAKE UP did nothing when tracking was off, fixed |
| Water | Working | no undo / custom amount |
| Clock | Working (2.2.3) | stopwatch with laps, settable timer with pause, alarm with snooze |
| Alerts (list, detail, popup) | Working | - |
| Music | Working (needs iPhone AMS) | - |
| Settings (+ set time) | Working | date not settable (see 19) |
| BLE / diag | Working (developer screen) | - |

Automated tap check (tests/uisnap, `make uicheck`): every button on 26 screen
states (clock tabs in every state, water, workout pick/run/done, sleep, all
three settings pages, set time, alerts list and detail, music, diag) is tapped
through the real UI code and must change the screen or the state behind it.
Result: 105 buttons tapped, 0 dead. The five music buttons show no change on
their own because they send commands to the iPhone; they are listed, not
failed. The launcher, control centre and stats grid are canvases with their own
hit tables, checked by the existing canvas audit. Found and fixed by it: WAKE UP
on the sleep screen did nothing when sleep tracking was switched off.

## Makeshift icons to replace (Material Symbols / Fluent equivalents)

| Glyph | Used on | Material Symbols | Fluent |
|-------|---------|------------------|--------|
| G_WORKOUT | launcher | fitness_center | Dumbbell |
| G_STATS | launcher | monitoring / bar_chart | DataBarVertical |
| G_CARDIO | launcher, stats, cardio, face heart | favorite / ecg_heart | Heart / HeartPulse |
| G_SLEEP | launcher, stats, CC DND | bedtime | WeatherMoon |
| G_WATER | launcher, stats, CC water lock, face chip | water_drop | Drop |
| G_CLOCK | launcher | schedule / alarm | Clock / ClockAlarm |
| G_ALERTS | launcher | notifications | Alert |
| G_SETTINGS | launcher, CC | settings | Settings |
| G_DIAG | stats REST tile | monitor_heart | HeartPulse |
| G_PHONE | CC alerts | smartphone | Phone |
| G_BATT | CC | battery_full / battery_charging_full | Battery10 / BatteryCharge |
| G_SUN | CC brightness | brightness_high | BrightnessHigh |
| G_BT | CC | bluetooth | Bluetooth |
| G_PLANE | CC | airplanemode_active | Airplane |
| G_TORCH | CC | flashlight_on | Flashlight |
| G_STEPS | stats | directions_walk | Walking |
| G_BOLT | stats ENERGY | bolt | Flash |
| DK_M_* (play, pause, next, prev, vol up, vol down) | music screen and face chip | play_arrow, pause, skip_next, skip_previous, volume_up, volume_down | Play, Pause, Next, Previous, Speaker2, Speaker1 |

## Fix log for 2.2.3

- Battery %: 64x oversampled SAADC with offset calibration every 6 h, 40 us
  acquisition, read about once a minute (30 s on the charger), never while the
  motor runs; load compensation for backlight and HR LED; median of three, slow
  low-pass, PineTime LiPo curve; shown value moves 1 % at a time, only down on
  battery and only up on the charger; one correction after plug/unplug once it
  settles. Watch, InfiniLink and the iPhone all read the same filtered value.
  tests/test_batt.c: steady noisy charge, 40 h discharge with load spikes,
  charging, plug/unplug.
- Drain: CPU sleeps (WFE), DC/DC, HR duty cycle, BLE intervals, accel rate,
  updaters stopped with the screen off, ANCS retry backoff, screen-off idle
  20 ms -> 90 ms with button wake. Screen-off main-loop wake-ups, from the loop
  timing (not measured on a watch): 2.2.2 never slept at all (busy-wait);
  2.2.3 WIP ~40 per second (~2,400/min); 2.2.3 ~11 per second (~650/min), plus
  the radio's own events.
- Clock: stopwatch with tenths and laps; timer 1-99 min with hold-to-repeat,
  pause/resume, opens itself and pulses until dismissed (stops after 1 min),
  +1 MIN; alarm in 1-minute steps (hold = 5), shows how long until it rings,
  SNOOZE 9 min, rings with the screen off, snoozes itself after a minute
  unanswered and gives up after three.
- Settings: power-loss-safe save (two CRC-checked slots); a 2.2.2 record still
  loads.
- HR double press: 350 ms window, two buzzes on / one off, HR ON / HR OFF
  toast, sensor powered down, no stale numbers on face/cardio/stats, resting HR
  skips the gap, a running workout keeps HR on ("HR IN USE") and starting one
  turns HR back on; saved across reboots. tests/test_btn.c.
