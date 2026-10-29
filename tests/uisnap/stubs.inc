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
int hal_clock_hour(void) { return 10; }
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
uint32_t hal_clock_tod_ms(void) { return 630u * 60000u; }
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
static int snap_notif_stress;   /* 6 notifications, every field full */
static int snap_notif_none;     /* nothing on the phone */
static uint32_t snap_popup;     /* uid whose text just arrived */
int ancs_count(void)
{
    return snap_notif_none ? 0 : snap_notif_stress ? 6 : 3;
}
int ancs_take_dirty(void) { return 0; }
int ancs_take_new(void) { return 0; }
uint32_t ancs_take_popup(void) { uint32_t u = snap_popup; snap_popup = 0; return u; }
void ancs_dismiss(uint32_t u) { (void)u; }
void ancs_positive(uint32_t u) { (void)u; }
const struct ancs_notif_view *ancs_get(int i)
{
    /* uid flags pos neg cat app title sub msg rx date mod */
    static struct ancs_notif_view n[3] = {
        {100, 0, 1, 1, 4, "Messages", "Sam", "",
         "are we still on for tonight? I can bring food", 99000, 20261028, 9 * 60 + 41},
        {101, 0, 0, 1, 6, "Mail", "GitHub", "PR opened: fix heart rate",
         "andyjxck opened a pull request in voidoisnew", 80000, 20261028, 8 * 60 + 5},
        {102, 0, 0, 1, 5, "Calendar", "Standup", "",
         "10:00 AM - 10:15 AM", 50000, 20261027, 17 * 60 + 30},
    };
    /* ancs_store.h limits: 6 notifications, app 19 / title 40 /
       subtitle 32 / message 119 chars, after folding to ASCII */
    static struct ancs_notif_view w[6] = {
        {200, 0, 1, 1, 4, "WA Business", "Samantha Wellington-Brown and seven oth",
         "Saturday plans with everyone xx",
         "are we still on for tonight? I can bring food if you want, or we could order in. Let me know!! Also bring the speaker pls",
         99000, 20261028, 23 * 60 + 59},
        {201, 0, 1, 1, 4, "WhatsApp", "Family group chat with a very long name",
         "Mum", "did everyone see the photos from the weekend? I put them all in the shared album for you, and the videos too - WWWWWWW",
         90000, 20261028, 12 * 60},
        {202, 0, 1, 1, 6, "Outlook", "[voidoisnew] Pull request opened: fix h",
         "Re: Re: Re: release notes for 2.2",
         "andyjxck wants to merge 12 commits into main from voidos-2.2: steadier heart rate and free RAM for the notification list ok",
         80000, 20261028, 0},
        {203, 0, 1, 1, 5, "Google Cal", "Standup with the whole team and the stak",
         "", "10:00 AM - 10:15 AM, Meeting room 4 on the second floor, bring the release notes and laptop, and the WWWWWWWWWWWWWWWWWW",
         50000, 20261027, 10 * 60},
        {204, 0, 0, 1, 1, "Phone", "Missed call", "", "+44 7700 900123",
         40000, 0, -1},
        {205, 0, 0, 1, 0, "Sesame", "", "",
         "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
         30000, 20261020, 6 * 60 + 15},
    };
    if (snap_notif_none)
        return NULL;
    if (snap_notif_stress)
        return (i >= 0 && i < 6) ? &w[i] : NULL;
    return (i >= 0 && i < 3) ? &n[i] : NULL;
}

/* phone-fed data (music), switchable per shot */
static int snap_compat;
void ble_set_compat(int on) { snap_compat = on; }
int ble_get_compat(void) { return snap_compat; }
static struct music snap_music_data = {
    .playing = 1, .src = MS_SRC_AMS, .artist = "Daft Punk",
    .track = "Digital Love", .album = "Discovery",
    .pos_s = 97, .len_s = 301, .pos_ms = 100000};
const struct music *music_get(void) { return &snap_music_data; }
static int snap_music_recent = 1;   /* paused less than 10 min ago */
int music_active(void)
{
    return ble_connected() && (snap_music_data.playing ||
                               (snap_music_data.track[0] && snap_music_recent));
}
void music_cmd(int c) { (void)c; }
int music_vol_ok(void) { return 1; }
int phone_take_dirty(void) { return 0; }
int phone_take_face_dirty(void) { return 0; }
