static uint32_t now_ms = 100000;
static int snap_bpm = 72, snap_hr_on = 1, snap_stale = 0;
uint32_t hal_millis_raw(void) { return now_ms; }
uint32_t hal_millis(void) { return now_ms; }
void fault_blink(int p) { fprintf(stderr, "LVGL assert (fault_blink %d)\n", p); abort(); }
void display_backlight(int l) { (void)l; }
void display_sleep(int on) { (void)on; }
void touch_sleep(int on) { (void)on; }
void sensors_tick(uint32_t n) { (void)n; }
int sensors_bma_ok(void) { return 1; }
int sensors_bma_z(void) { return 900; }
int sensors_hrs_ok(void) { return 1; }
int sensors_steps(void) { return 6420; }
int sensors_bpm(void) { return snap_hr_on ? snap_bpm : 0; }
int sensors_bpm_stale(void) { return snap_hr_on && snap_bpm > 0 && snap_stale; }
int sensors_hr_on(void) { return snap_hr_on; }
void sensors_hr_set(int on) { snap_hr_on = on; }
int sensors_wrist_raise(void) { return 0; }
int sensors_raise_wake(void) { return 0; }
int sensors_ppg(int i, int *y) { *y = 5000 + 3000 * (((i * 7) % 20) > 10 ? 1 : -1); return 1; }
int sensors_hrs_raw(void) { return 18000; }
int sensors_hrs_ac(void) { return 60; }
int sensors_hrs_fs10(void) { return 100; }
int sensors_hrs_snr10(void) { return 52; }
int sensors_hrs_windows(void) { return 30; }
int sensors_retries(void) { return 0; }
int sensors_bma_engine(void) { return 1; }
int sensors_bma_dbg(void) { return 0; }
int sensors_hrs_pk(void) { return 72; }
int sensors_hrs_als(void) { return 300; }
int hal_clock_hour(void) { return 0; }
int hal_clock_minute(void) { return 30; }
int hal_clock_second(void) { return 12; }
int hal_clock_millis(void) { return 0; }
void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s)
{ (void)y; (void)mo; (void)d; (void)dow; (void)h; (void)mi; (void)s; }
void hal_clock_date(int *y, int *mo, int *d, int *dow)
{ *y = 2026; *mo = 10; *d = 28; *dow = 3; }
static int snap_synced = 1, snap_restored;
int hal_clock_synced(void) { return snap_synced; }
int hal_clock_restored(void) { return snap_restored; }
void hal_clock_restore(int y, int mo, int d, int dow, int h, int mi, int s)
{ (void)y; (void)mo; (void)d; (void)dow; (void)h; (void)mi; (void)s; }
uint32_t hal_clock_tod_ms(void) { return 30 * 60000; }
int hal_battery_pct(void) { return 81; }
int hal_battery_raw(void) { return 3300; }
int hal_boot_confirmed(void) { return 1; }
void hal_motor(int on) { (void)on; }
uint32_t button_held_ms(void) { return 0; }
static int snap_ble = 1;
int ble_connected(void) { return snap_ble; }
void ble_set_enabled(int on) { (void)on; }
int ble_status(void) { return 2; }
int ble_dfu_busy(void) { return 0; }
void ble_link_diag(int *a, int *b, int *c, int *d, int *e)
{ *a = *b = *c = *d = *e = 0; }
void spi_flash_init(void) {}
void spi_flash_read(uint32_t a, uint8_t *b, unsigned n) { (void)a; memset(b, 0xFF, n); }
void spi_flash_write(uint32_t a, const uint8_t *b, unsigned n) { (void)a; (void)b; (void)n; }
void spi_flash_sector_erase(uint32_t a) { (void)a; }
int ancs_cts_state(void) { return 0; }
int ancs_sub_fail(void) { return 0; }
void ble_bond_diag(int *a, int *b) { *a = 1; *b = 0; }
int ams_state(void) { return 7; }
int ancs_seq_phase(void) { return 4; }
int ancs_ready(void) { return 1; }
int ancs_state(void) { return 3; }
static int snap_notif_stress;   /* 4 notifications, every field full */
int ancs_count(void) { return snap_notif_stress ? 4 : 3; }
int ancs_take_dirty(void) { return 0; }
int ancs_take_new(void) { return 0; }
void ancs_dismiss(uint32_t u) { (void)u; }
void ancs_positive(uint32_t u) { (void)u; }
const struct ancs_notif_view *ancs_get(int i)
{
    static struct ancs_notif_view n[3] = {
        {100, 0, 1, 1, "Messages", "Sam", "are we still on for tonight? I can bring food", 99000},
        {101, 0, 0, 1, "Mail", "GitHub", "[voidoisnew] PR opened: fix heart rate", 80000},
        {102, 0, 0, 1, "Calendar", "Standup", "10:00 AM - 10:15 AM", 50000},
    };
    /* ancs.c limits: 4 notifications, app 19 / title 47 / msg 95 chars */
    static struct ancs_notif_view w[4] = {
        {200, 0, 1, 1, "Messages Messages M", "Samantha Wellington-Brown and seven other peop",
         "are we still on for tonight? I can bring food if you want, or we could order in. Let me know!!", 99000},
        {201, 0, 1, 1, "WhatsApp WhatsApp W", "Family group chat with a very long name indeed",
         "Mum: did everyone see the photos from the weekend? I put them all in the shared album for you", 90000},
        {202, 0, 1, 1, "Mail Mail Mail Mail", "[voidoisnew] Pull request opened: fix heart ra",
         "andyjxck wants to merge 12 commits into main from voidos-2.2: steadier heart rate and free RAM", 80000},
        {203, 0, 1, 1, "Calendar Calendar C", "Standup with the whole team and the stakehold",
         "10:00 AM - 10:15 AM, Meeting room 4 on the second floor, bring the release notes and laptop", 50000},
    };
    if (snap_notif_stress)
        return (i >= 0 && i < 4) ? &w[i] : NULL;
    return (i >= 0 && i < 3) ? &n[i] : NULL;
}

/* phone-fed data (weather + music), switchable per shot */
static int snap_compat;
void ble_set_compat(int on) { snap_compat = on; }
int ble_get_compat(void) { return snap_compat; }
static struct wx snap_wx_data = {
    .temp = 1840, .tmin = 1120, .tmax = 2150, .icon = WX_FEWCLOUDS,
    .city = "Redditch", .rx_ms = 100000 - 12 * 60000, .ndays = 5,
    .day = {{1120, 2150, WX_FEWCLOUDS}, {980, 1760, WX_RAIN},
            {870, 1490, WX_SHOWERS}, {640, 1310, WX_CLOUDS},
            {720, 1580, WX_SUN}}};
static int snap_wx_on = 1;
const struct wx *wx_get(void) { return snap_wx_on ? &snap_wx_data : NULL; }
const char *wx_name(int icon)
{
    static const char *n[WX_N] = {"CLEAR", "FEW CLOUDS", "CLOUDY",
                                  "OVERCAST", "SHOWERS", "RAIN",
                                  "THUNDER", "SNOW", "MIST"};
    return icon >= 0 && icon < WX_N ? n[icon] : "--";
}
static struct music snap_music_data = {
    .playing = 1, .src = MS_SRC_AMS, .artist = "Daft Punk",
    .track = "Digital Love", .album = "Discovery",
    .pos_s = 97, .len_s = 301, .pos_ms = 100000};
const struct music *music_get(void) { return &snap_music_data; }
int music_active(void) { return snap_music_data.playing && ble_connected(); }
void music_cmd(int c) { (void)c; }
int music_vol_ok(void) { return 1; }
int phone_take_dirty(void) { return 0; }
int phone_take_face_dirty(void) { return 0; }
