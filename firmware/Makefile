# VoidOS PineTime firmware — bare-metal nRF52832, MCUBoot app slot
.DEFAULT_GOAL := all
# ~/toolchains/arm-gcc when present, else whatever arm-none-eabi-gcc is on PATH
TOOLCHAIN ?= $(if $(wildcard $(HOME)/toolchains/arm-gcc/bin),$(HOME)/toolchains/arm-gcc/bin,$(patsubst %/,%,$(dir $(shell command -v arm-none-eabi-gcc))))
TOOLS     ?= $(HOME)/.voidos-tools/bin
CROSS     ?= $(TOOLCHAIN)/arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size
PYTHON  = $(TOOLS)/python3

ARCH    = -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS  = $(ARCH) -std=c11 -Os -g -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -I../include -I../core -I. \
          -DVOIDOS_DEVICE -DVOIDOS_DEBUG_TOUCH -DVOIDOS_VERSION=\"$(VERSION)\"
LDFLAGS = $(ARCH) -T link.ld -Wl,--gc-sections -Wl,-Map=voidos-app.map \
          -specs=nano.specs -Wl,--no-warn-rwx-segments

VERSION ?= 2.0.0
BIN      = voidos-app.bin
IMG      = voidos-app-image.bin
ZIP      = voidos-dfu-$(VERSION).zip

LVGL_SRC := $(shell find vendor/lvgl/src -name '*.c')
LVGL_OBJS := $(LVGL_SRC:.c=.o)
LVGL_CFLAGS = $(ARCH) -std=gnu11 -Os -g -ffunction-sections -fdata-sections \
              -w -DLV_CONF_INCLUDE_SIMPLE -DLV_USE_OS=LV_OS_NONE -Ivendor -I.

OBJS = startup.o main.o syscalls.o hal_sys.o hal_display.o lvgl_port.o \
       ui_face.o ui_nav.o sensors.o spi_flash.o \
       nimble_port/src/ble_dfu.o nimble_port/src/ancs.o \
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
       -include syscfg/syscfg.h -include os/util.h $(NIMBLE_INC)

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

voidos-app.out: $(OBJS) link.ld
	$(CC) $(OBJS) $(LDFLAGS) -lm -o $@

$(BIN): voidos-app.out
	$(OBJCOPY) -O binary $< $@

# MCUBoot image: 32-byte header, app slot 475136 bytes at 0x8000.
# Same imgtool arguments as InfiniTime's build (--align 1).
$(IMG): $(BIN)
	$(TOOLS)/imgtool create --align 1 --version $(VERSION) --header-size 32 \
	    --slot-size 475136 --pad-header $< $@

$(ZIP): $(IMG)
	$(TOOLS)/adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application $< $@

size: voidos-app.out
	$(SIZE) $<

clean:
	rm -f *.o gen/*.o *.out *.map $(BIN) $(IMG) voidos-dfu-*.zip

.PHONY: all clean size
