static uint32_t now_ms = 100000;
static int snap_bpm = 72, snap_hr_on = 1;
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
int hal_clock_synced(void) { return 1; }
uint32_t hal_clock_tod_ms(void) { return 30 * 60000; }
int hal_battery_pct(void) { return 81; }
int hal_battery_raw(void) { return 3300; }
int hal_boot_confirmed(void) { return 1; }
void hal_motor(int on) { (void)on; }
uint32_t button_held_ms(void) { return 0; }
int ble_connected(void) { return 1; }
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
int ancs_ready(void) { return 1; }
int ancs_state(void) { return 3; }
int ancs_count(void) { return 3; }
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
    return (i >= 0 && i < 3) ? &n[i] : NULL;
}
