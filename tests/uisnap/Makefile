FW = ../../firmware
LVGL_SRC := $(shell find $(FW)/vendor/lvgl/src -name '*.c')
CFLAGS = -m32 -O1 -g -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_USE_OS=LV_OS_NONE \
         -I$(FW)/vendor -I$(FW)/vendor/lvgl -I$(FW) -I../../include -I../../core -I$(FW)/nimble_port/include -DUISNAP_SHAPES
OBJDIR = build
LVGL_OBJS := $(patsubst $(FW)/%.c,$(OBJDIR)/%.o,$(LVGL_SRC))

snap: snap.c stubs.inc $(FW)/ui_nav.c $(FW)/ui_face.c $(FW)/ui_phone.inc $(FW)/ui_hist.inc $(FW)/hlog.c $(FW)/hlog.h $(FW)/ui_deco.h $(FW)/ancs_view.h $(FW)/daily.h $(FW)/goal.h $(FW)/ui_goal.inc $(FW)/ui_charge.inc $(FW)/ui_power.inc $(FW)/alarm.h $(FW)/phone.h $(FW)/micons.h $(LVGL_OBJS) $(OBJDIR)/gen/kenney_lvgl.o $(OBJDIR)/gen/assets_data.o $(OBJDIR)/gen/micons.o
	$(CC) $(CFLAGS) -m32 -rdynamic -o $@ snap.c $(FW)/ui_face.c $(FW)/hlog.c $(LVGL_OBJS) $(OBJDIR)/gen/kenney_lvgl.o $(OBJDIR)/gen/assets_data.o $(OBJDIR)/gen/micons.o -lm

# every LVGL object depends on its config (strip height, heap size)
$(LVGL_OBJS): $(FW)/lv_conf.h

$(OBJDIR)/%.o: $(FW)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) snap
