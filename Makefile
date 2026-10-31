CC ?= clang
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
TTF_PREFIX := $(shell brew --prefix sdl2_ttf 2>/dev/null || echo /usr)
TTF_CFLAGS := -I$(TTF_PREFIX)/include
TTF_LIBS := -L$(TTF_PREFIX)/lib -lSDL2_ttf
IMG_PREFIX := $(shell brew --prefix sdl2_image 2>/dev/null || echo /usr)
IMG_CFLAGS := -I$(IMG_PREFIX)/include
IMG_LIBS := -L$(IMG_PREFIX)/lib -lSDL2_image
CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
CORE_SRC := core/app.c core/font.c

all: build/voidos-sim

build/voidos-sim: $(CORE_SRC) simulator/main.c simulator/sdl_hal.c
	mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) $(IMG_CFLAGS) $^ -o $@ $(SDL_LIBS) $(TTF_LIBS) $(IMG_LIBS) -lm

build/test_app: $(CORE_SRC) tests/test_app.c
	mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ -lm

build/test_screenshot: simulator/sdl_hal.c tests/test_screenshot.c
	mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) $(IMG_CFLAGS) $^ -o $@ $(SDL_LIBS) $(TTF_LIBS) $(IMG_LIBS)

build/test_input: $(CORE_SRC) simulator/sdl_hal.c tests/test_input.c
	mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) $(IMG_CFLAGS) $^ -o $@ $(SDL_LIBS) $(TTF_LIBS) $(IMG_LIBS) -lm

build/test_ble_dfu: tests/dfu/test_ble_dfu.c firmware/nimble_port/src/ble_dfu.c
	mkdir -p build
	$(CC) $(CFLAGS) -Itests/dfu/stubs -Ifirmware tests/dfu/test_ble_dfu.c -o $@

# HR pipeline: the real firmware/sensors.c HRS3300 code on synthetic PPG
TINYCRYPT = firmware/vendor/nimble/ext/tinycrypt
build/test_ble_units: tests/ble/test_ble_units.c firmware/nimble_port/src/gatt_owner.h firmware/nimble_port/src/ams_parse.h firmware/nimble_port/src/bondblob.c firmware/nimble_port/src/bondblob.h firmware/nimble_port/src/rpa_match.h
	mkdir -p build
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Ifirmware -I$(TINYCRYPT)/include tests/ble/test_ble_units.c $(TINYCRYPT)/src/aes_encrypt.c $(TINYCRYPT)/src/utils.c -o $@

# 2.2.2: notification store, app names, text folding, midnight reset
build/test_store_daily: tests/ble/test_store_daily.c firmware/nimble_port/src/ancs_store.h firmware/daily.h firmware/goal.h
	mkdir -p build
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Wno-unused-function -Ifirmware tests/ble/test_store_daily.c -o $@

# phone history outbox (firmware/hlog.c) on a simulated NOR flash
build/test_hlog: tests/hlog/test_hlog.c firmware/hlog.c firmware/hlog.h
	@mkdir -p build
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Ifirmware tests/hlog/test_hlog.c firmware/hlog.c -o $@

build/test_hr: tests/hr/test_hr.c firmware/sensors.c
	mkdir -p build
	$(CC) -std=gnu11 -O1 -w -Iinclude -Icore -Ifirmware tests/hr/test_hr.c -o $@ -lm

# same scenarios against the 2.1.0 HR code, for a before/after picture
build/sensors_2_1.c:
	mkdir -p build
	git show 779232a4:firmware/sensors.c > $@

build/test_hr_old: tests/hr/test_hr.c build/sensors_2_1.c
	$(CC) -std=gnu11 -O1 -w -Iinclude -Icore -Ifirmware -DHR_OLD \
	    -DSENSORS_C='"../../build/sensors_2_1.c"' tests/hr/test_hr.c -o $@ -lm

hrcompare: build/test_hr build/test_hr_old
	./build/test_hr_old | grep '^\['
	./build/test_hr | grep '^\['

test: build/test_app build/test_screenshot build/test_input build/test_ble_dfu build/test_hr build/test_ble_units build/test_store_daily build/test_btn build/test_batt build/test_hlog
	./build/test_app
	SDL_VIDEODRIVER=dummy ./build/test_screenshot
	SDL_VIDEODRIVER=dummy ./build/test_input
	./build/test_ble_dfu
	./build/test_hr
	./build/test_ble_units
	./build/test_store_daily
	./build/test_btn
	./build/test_batt
	./build/test_hlog

# everything: host tests plus the rendered-screen checks
check: test uicheck

# renders every watch screen and fails on small/overlapping tap targets
# or clipped text (writes PPMs + taps.txt to build/uisnap)
uicheck:
	$(MAKE) -C tests/uisnap
	mkdir -p build/uisnap
	./tests/uisnap/snap build/uisnap

# smallest LVGL heap that renders every screen identically (see script)
heapcheck:
	./tests/uisnap/heapcheck.sh

clean:
	rm -rf build/voidos-sim build/test_app build/test_screenshot build/test_input build/test_ble_dfu build/test_hr build/test_ble_units build/test_store_daily build/test_hr_old build/sensors_2_1.c build/uisnap
	-rmdir build 2>/dev/null || true

.PHONY: all test check uicheck clean hrcompare heapcheck

build/test_batt: tests/test_batt.c firmware/batt.h
	mkdir -p build
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Werror $< -o $@

build/test_btn: tests/test_btn.c firmware/btn_sm.h
	mkdir -p build
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Werror $< -o $@
