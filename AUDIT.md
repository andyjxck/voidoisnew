# VoidOS whole-watch audit (for 2.2.3)

Base: branch `voidos-2.2.2` plus the 2.2.3 work in progress. Method: reading the
firmware source (ui_nav.c, ui_face.c, hal_sys.c, sensors.c, spi_flash.c,
ble_app.c, ancs.c, main.c), the host tests, and the uisnap screen renderer.
Nothing here was measured on a real watch. Each finding is ranked by how much it
hurts day-to-day use. Status column: FIXED (in 2.2.3), PARTLY, OPEN.

## Ranked findings

| # | Area | Finding | Status |
|---|------|---------|--------|
| 1 | Battery % | One raw 12-bit SAADC sample per read, no oversampling, no filter, read while the screen/backlight/motor/HR LED load the cell, so the number jumps (26 > 30 > 25 > 29). Each consumer (control centre every second, diag, BLE battery read) took its own fresh sample, so the watch and the phone disagreed. | see below |
| 2 | Battery drain | CPU never slept: `hal_delay_ms` spun on the RTC counter at 64 MHz around the clock. | FIXED (WFE sleep, in the 2.2.3 WIP) |
| 3 | Battery drain | Core ran on the LDO, not the DC/DC converter the PineTime has. | FIXED (DCDCEN) |
| 4 | Battery drain | HR LED on 24/7 (screen off, on the charger, off the wrist). | FIXED (live only with screen on or workout; background reading every 5 min, none on charger) |
| 5 | Battery drain | Advertising at NimBLE's 30-60 ms default forever; iOS link at 30 ms, no latency. | FIXED (fast 30 s then 1022.5 ms; 45-60 ms latency 3 after 10 s) |
| 6 | Battery drain | Screen-off main loop still woke ~40 times a second (20 ms + 5 ms waits), each pass polling I2C. | see below |
| 7 | Battery drain | SPI NOR flash woken at boot/settings save and never put back into deep power-down (0xB9). | see below |
| 8 | Battery drain | Accel sampled at 50 Hz and the Bosch step counter read 50 times a second with the screen off; wrist-raise register read every loop. | FIXED (10 Hz asleep, counter 1 Hz, raise 10 Hz) |
| 9 | Battery drain | Per-screen updaters (second hand, heart, music clock, cardio trace) ran while the panel slept. | FIXED |
| 10 | Battery drain | ANCS retry storm: full GATT discovery every 60 s forever while iOS refused notification access. | FIXED (every 5 min after ~20 min) |
| 11 | Dead/limited screens | CLOCK: stopwatch only start/reset, no laps; timer only 1/5/10 min presets with no custom time; alarm settable only in 15-minute steps, no snooze, rings forever if you are not there, never rings until the clock has been set. Feels like it does nothing useful. | see below |
| 12 | Reliability | Settings live in one flash sector with no checksum: erase-then-write, so a reset or flat battery mid-save loses every setting and the step total. | see below |
| 13 | Reliability | Settings save does a 4 KB sector erase on the UI thread (up to ~300 ms freeze); deferred 2 s after a change, so it lands while you are still tapping. | OPEN (deferred, not moved off the UI thread) |
| 14 | UX | Side button single press waited for nothing; adding the double press means a single press on an awake screen now waits 300 ms. | ACCEPTED (asleep presses act at once) |
| 15 | Feature | Double press of the side button toggles the HR sensor. | see below |
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
| Sleep (+ night summary) | Partly working | manual only (see 16) |
| Water | Working | no undo / custom amount |
| Clock | Partly working, feels dead | see 11 |
| Alerts (list, detail, popup) | Working | - |
| Music | Working (needs iPhone AMS) | - |
| Settings (+ set time) | Working | date not settable (see 19) |
| BLE / diag | Working (developer screen) | - |

The automated tap check (every clickable object on every screen is tapped in
the host renderer and must change something) is added in 2.2.3; its results are
filled in below once it runs.

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

(updated as work lands)
