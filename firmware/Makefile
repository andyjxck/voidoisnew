# VoidOS PineTime firmware — bare-metal nRF52832, MCUBoot app slot
.DEFAULT_GOAL := all
# ~/toolchains/arm-gcc when present, else whatever arm-none-eabi-gcc is on PATH
TOOLCHAIN ?= $(if $(wildcard $(HOME)/toolchains/arm-gcc/bin),$(HOME)/toolchains/arm-gcc/bin,$(patsubst %/,%,$(dir $(shell command -v arm-none-eabi-gcc))))
TOOLS     ?= $(HOME)/.voidos-tools/bin
CROSS     ?= $(TOOLCHAIN)/arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size
OBJDUMP = $(CROSS)objdump
NM      = $(CROSS)nm
PYTHON  = $(TOOLS)/python3

ARCH    = -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS  = $(ARCH) -std=c11 -Os -g -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -I../include -I../core -I. \
          -DVOIDOS_DEVICE -DVOIDOS_DEBUG_TOUCH $(DEPFLAGS)
# Every object records the headers it includes (.d files, read at the end),
# so editing a header or an .inc rebuilds what uses it.
DEPFLAGS = -MMD -MP
LDFLAGS = $(ARCH) -T link.ld -Wl,--gc-sections -Wl,-Map=voidos-app.map \
          -specs=nano.specs -Wl,--no-warn-rwx-segments

VERSION ?= 2.2.2
BIN      = voidos-app.bin
IMG      = voidos-app-image.bin
ZIP      = voidos-dfu-$(VERSION).zip

LVGL_SRC := $(shell find vendor/lvgl/src -name '*.c')
LVGL_OBJS := $(LVGL_SRC:.c=.o)
LVGL_CFLAGS = $(ARCH) -std=gnu11 -Os -g -ffunction-sections -fdata-sections \
              -w -DLV_CONF_INCLUDE_SIMPLE -DLV_USE_OS=LV_OS_NONE -Ivendor -I. $(DEPFLAGS)

OBJS = startup.o main.o syscalls.o hal_sys.o hal_display.o lvgl_port.o \
       ui_face.o ui_nav.o sensors.o spi_flash.o \
       nimble_port/src/ble_dfu.o nimble_port/src/ancs.o nimble_port/src/ble_svcs.o nimble_port/src/ams.o \
       nimble_port/src/ble_bond_store.o nimble_port/src/bondblob.o \
       gen/kenney_lvgl.o gen/assets_data.o $(LVGL_OBJS) \
       nimble_port/src/npl_baremetal.o nimble_port/src/glue.o \
       nimble_port/src/ble_app.o $(NIMBLE_OBJS)

# Apache NimBLE: host + controller + nRF52 driver + transport + porting layer
NIMBLE_DIRS = vendor/nimble/nimble/host/src vendor/nimble/nimble/host/util/src \
       vendor/nimble/nimble/host/services/gap/src \
       vendor/nimble/nimble/host/services/gatt/src \
       vendor/nimble/nimble/host/store/ram/src \
       vendor/nimble/nimble/controller/src \
       vendor/nimble/nimble/drivers/nrf5x/src \
       vendor/nimble/nimble/drivers/nrf5x/src/nrf52 \
       vendor/nimble/nimble/transport/src \
       vendor/nimble/porting/nimble/src \
       vendor/nimble/ext/tinycrypt/src
NIMBLE_SRC  := $(foreach d,$(NIMBLE_DIRS),$(wildcard $(d)/*.c))
NIMBLE_OBJS := $(NIMBLE_SRC:.c=.o)
NIMBLE_INC  = -Inimble_port/include \
       -Ivendor/nimble/nimble/include -Ivendor/nimble/nimble/host/include \
       -Ivendor/nimble/nimble/host/util/include \
       -Ivendor/nimble/nimble/host/services/gap/include \
       -Ivendor/nimble/nimble/host/services/gatt/include \
       -Ivendor/nimble/nimble/host/store/ram/include \
       -Ivendor/nimble/nimble/controller/include \
       -Ivendor/nimble/nimble/drivers/nrf5x/include \
       -Ivendor/nimble/nimble/transport/include \
       -Ivendor/nimble/porting/nimble/include \
       -Ivendor/nimble/ext/tinycrypt/include \
       -Ivendor/nrfx -Ivendor/nrfx/mdk \
       -Ivendor/nrfx/templates -Ivendor/cmsis/CMSIS/Core/Include -I.
NIMBLE_CFLAGS = $(ARCH) -std=gnu11 -Os -g -ffunction-sections -fdata-sections \
       -w -DNRF52832_XXAA -DNIMBLE_CFG_CONTROLLER \
       -include syscfg/syscfg.h -include os/util.h $(NIMBLE_INC) $(DEPFLAGS)

$(NIMBLE_OBJS): %.o: %.c nimble_port/include/syscfg/syscfg.h
	$(CC) $(NIMBLE_CFLAGS) -c $< -o $@

nimble_port/%.o: nimble_port/%.c nimble_port/include/syscfg/syscfg.h
	$(CC) $(CFLAGS) $(NIMBLE_INC) -Wno-error -c $< -o $@

nimble_port/include/syscfg/syscfg.h: tools/gen_syscfg.py
	$(PYTHON) tools/gen_syscfg.py

all: $(ZIP) size

gen/assets_data.c gen/kenney_lvgl.c &: tools/gen_assets.py ../assets/fonts/Kenney\ Future.ttf
	$(PYTHON) tools/gen_assets.py

gen/kenney_lvgl.o: gen/kenney_lvgl.c lv_conf.h
	$(CC) $(CFLAGS) -Ivendor -Ivendor/lvgl -DLV_CONF_INCLUDE_SIMPLE -c $< -o $@

gen/assets_data.o: gen/assets_data.c assets.h
	$(CC) $(CFLAGS) -c $< -o $@

core_app.o: ../core/app.c
	$(CC) $(CFLAGS) -Wno-format-truncation -c $< -o $@

core_font.o: ../core/font.c
	$(CC) $(CFLAGS) -c $< -o $@

vendor/lvgl/src/%.o: vendor/lvgl/src/%.c lv_conf.h
	$(CC) $(LVGL_CFLAGS) -c $< -o $@

%.o: %.c board.h
	$(CC) $(CFLAGS) -Ivendor -Ivendor/lvgl -DLV_CONF_INCLUDE_SIMPLE -c $< -o $@

# gen/version.h carries VERSION. It is rewritten only when VERSION
# changes, and the .d files make everything that includes it rebuild, so
# a version bump can never leave a stale "VOIDOS x.y.z" string behind.
# gen/flags.stamp does the same for the compiler flags themselves.
FLAGS_SIG = $(CC) $(CFLAGS) $(LVGL_CFLAGS) $(NIMBLE_CFLAGS)
gen/version.h: FORCE
	@mkdir -p gen
	@printf '#define VOIDOS_VERSION "%s"\n' '$(VERSION)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "version.h -> $(VERSION)"; fi
gen/flags.stamp: FORCE
	@mkdir -p gen
	@echo '$(FLAGS_SIG)' | md5sum > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "compiler flags changed: full rebuild"; fi
$(OBJS): gen/flags.stamp | gen/version.h

# hand-written peripheral register offsets must match the Nordic MDK
gen/regcheck.ok: hal_regcheck.c hal_saadc.h
	@mkdir -p gen
	$(CC) $(ARCH) -std=c11 -fsyntax-only -Wall -Werror -DNRF52832_XXAA \
	    -Ivendor/nrfx/mdk -Ivendor/cmsis/CMSIS/Core/Include -I. $< && touch $@

voidos-app.out: $(OBJS) link.ld gen/regcheck.ok
	$(CC) $(OBJS) $(LDFLAGS) -lm -o $@

# The .bin is flashed right after the 32-byte MCUBoot header, so its
# first byte must be the vector table at 0x8020 and the table's reset
# vector must be Reset_Handler. Refuse to package an image that would
# be shifted (the 2.0.0 boot loop).
$(BIN): voidos-app.out
	$(OBJCOPY) -O binary $< $@
	@first=$$($(OBJDUMP) -h $< | awk '$$2!~/^\.(data|bss)$$/ && $$4 ~ /^0000[0-9a-f]+$$/ && $$3!="00000000" {print $$4; exit}'); \
	vec=$$($(NM) $< | awk '$$3=="vectors"{print $$1}'); \
	rst=$$($(NM) $< | awk '$$3=="Reset_Handler"{print $$1}'); \
	word=$$(od -An -tx4 -j4 -N4 $@ | tr -d ' '); \
	if [ "$$first" != "00008020" ] || [ "$$vec" != "00008020" ] || \
	   [ $$((0x$$word & ~1)) -ne $$((0x$$rst)) ]; then \
	  echo "image layout check FAILED: first section $$first, vectors $$vec, reset vector $$word, Reset_Handler $$rst"; \
	  rm -f $@; exit 1; fi; \
	echo "image layout OK: vectors at 0x$$vec, reset vector 0x$$word"

# MCUBoot image: 32-byte header, app slot 475136 bytes at 0x8000.
# --align 4 as in 1.6/1.6.2, the builds that booted on the watch.
$(IMG): $(BIN)
	$(TOOLS)/imgtool create --align 4 --version $(VERSION) --header-size 32 \
	    --slot-size 475136 --pad-header $< $@

$(ZIP): $(IMG)
	$(TOOLS)/adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application $< $@

size: voidos-app.out
	$(SIZE) $<

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) *.o *.d gen/*.o gen/*.d gen/version.h gen/flags.stamp \
	      *.out *.map $(BIN) $(IMG) voidos-dfu-*.zip

FORCE:
.PHONY: all clean size FORCE

-include $(OBJS:.o=.d)
