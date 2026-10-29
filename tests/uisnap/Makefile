FW = ../../firmware
LVGL_SRC := $(shell find $(FW)/vendor/lvgl/src -name '*.c')
CFLAGS = -m32 -O1 -g -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_USE_OS=LV_OS_NONE \
         -I$(FW)/vendor -I$(FW)/vendor/lvgl -I$(FW) -I../../include -I../../core -I$(FW)/nimble_port/include
OBJDIR = build
LVGL_OBJS := $(patsubst $(FW)/%.c,$(OBJDIR)/%.o,$(LVGL_SRC))

snap: snap.c $(FW)/ui_nav.c $(FW)/ui_face.c $(LVGL_OBJS) $(OBJDIR)/gen/kenney_lvgl.o $(OBJDIR)/gen/assets_data.o
	$(CC) $(CFLAGS) -m32 -o $@ snap.c $(FW)/ui_face.c $(LVGL_OBJS) $(OBJDIR)/gen/kenney_lvgl.o $(OBJDIR)/gen/assets_data.o -lm

# every LVGL object depends on its config (strip height, heap size)
$(LVGL_OBJS): $(FW)/lv_conf.h

$(OBJDIR)/%.o: $(FW)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) snap
