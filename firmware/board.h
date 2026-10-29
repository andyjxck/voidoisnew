/* nRF52832 / PineTime register + pin definitions shared by the firmware */
#ifndef VOIDOS_FW_BOARD_H
#define VOIDOS_FW_BOARD_H

#include <stdint.h>

#ifndef REG32
#define REG32(addr) (*(volatile uint32_t *)(addr))
#endif
#define BIT(p) (1u << (p))

/* GPIO P0 */
#define GPIO_OUT        REG32(0x50000504)
#define GPIO_OUTSET     REG32(0x50000508)
#define GPIO_OUTCLR     REG32(0x5000050C)
#define GPIO_IN         REG32(0x50000510)
#define GPIO_DIRSET     REG32(0x50000518)
#define GPIO_DIRCLR     REG32(0x5000051C)
#define GPIO_LATCH      REG32(0x50000520)
#define GPIO_DETECTMODE REG32(0x50000524)
#define GPIO_PIN_CNF(n) REG32(0x50000700 + (n) * 4)

/* SPIM0 (shared instance 0 at 0x40003000) */
#define SPIM0_START      REG32(0x40003010)
#define SPIM0_STOP       REG32(0x40003014)
#define SPIM0_EVENTS_END REG32(0x40003118)
#define SPIM0_ENABLE     REG32(0x40003500)
#define SPIM0_PSEL_SCK   REG32(0x40003508)
#define SPIM0_PSEL_MOSI  REG32(0x4000350C)
#define SPIM0_PSEL_MISO  REG32(0x40003510)
#define SPIM0_FREQ       REG32(0x40003524)
#define SPIM0_RXD_PTR    REG32(0x40003534)
#define SPIM0_RXD_MAXCNT REG32(0x40003538)
#define SPIM0_TXD_PTR    REG32(0x40003544)
#define SPIM0_TXD_MAXCNT REG32(0x40003548)
#define SPIM0_CONFIG     REG32(0x40003554)

/* TWIM1 (shared instance 1 at 0x40004000) — touch / accel / HRS bus */
#define TWIM1_STARTRX        REG32(0x40004000)
#define TWIM1_STARTTX        REG32(0x40004008)
#define TWIM1_STOP           REG32(0x40004014)
#define TWIM1_EVENTS_STOPPED REG32(0x40004104)
#define TWIM1_EVENTS_ERROR   REG32(0x40004124)
#define TWIM1_EVENTS_LASTRX  REG32(0x4000415C)
#define TWIM1_EVENTS_LASTTX  REG32(0x40004160)
#define TWIM1_SHORTS         REG32(0x40004200)
#define TWIM1_ERRORSRC       REG32(0x400044C4)
#define TWIM1_ENABLE         REG32(0x40004500)
#define TWIM1_PSEL_SCL       REG32(0x40004508)
#define TWIM1_PSEL_SDA       REG32(0x4000450C)
#define TWIM1_FREQ           REG32(0x40004524)
#define TWIM1_RXD_PTR        REG32(0x40004534)
#define TWIM1_RXD_MAXCNT     REG32(0x40004538)
#define TWIM1_TXD_PTR        REG32(0x40004544)
#define TWIM1_TXD_MAXCNT     REG32(0x40004548)
#define TWIM1_ADDRESS        REG32(0x40004588)

/* CLOCK */
#define CLOCK_LFCLKSTART    REG32(0x40000008)
#define CLOCK_EVENTS_LFCLKSTARTED REG32(0x40000104)
#define CLOCK_LFCLKSRC      REG32(0x40000518)

/* RTC1 */
#define RTC1_START      REG32(0x40011000)
#define RTC1_CLEAR      REG32(0x40011008)
#define RTC1_COUNTER    REG32(0x40011504)
#define RTC1_PRESCALER  REG32(0x40011508)

/* WDT (started by the bootloader, 7 s, reload via RR0) */
#define WDT_RR0  REG32(0x40010600)
#define WDT_KICK 0x6E524635u

/* SCB */
#define SCB_AIRCR REG32(0xE000ED0C)
#define SCB_CPACR REG32(0xE000ED88)

/* PineTime pins */
#define PIN_SCK       2
#define PIN_MOSI      3
#define PIN_MISO      4
#define PIN_FLASH_CS  5
#define PIN_SDA       6
#define PIN_SCL       7
#define PIN_TP_RESET  10
#define PIN_BTN_IN    13
#define PIN_BL_L      14
#define PIN_BTN_OUT   15
#define PIN_VIBRA     16
#define PIN_LCD_DC    18
#define PIN_BL_M      22
#define PIN_BL_H      23
#define PIN_LCD_CS    25
#define PIN_LCD_RST   26
#define PIN_TP_INT    28

void wdt_kick(void);
void fault_blink(int pattern);

/* external SPI NOR flash + BLE DFU */
void spi_flash_init(void);
void spi_flash_read(uint32_t addr, uint8_t *buf, unsigned len);
void spi_flash_write(uint32_t addr, const uint8_t *buf, unsigned len);
void spi_flash_sector_erase(uint32_t addr);
void spi_flash_sector_erase_start(uint32_t addr);
int spi_flash_busy(void);
void ble_dfu_init(void);

/* touch bring-up diagnostics (rendered as an overlay by main) */
typedef struct {
    unsigned twi_ok, twi_err, twi_timeout, latches, reads;
    unsigned bus_lvl;          /* SDA/SCL input levels at init (bit0|bit1) */
    unsigned stall;            /* i2c phase that timed out: 1=TX 2=RX 3=STOP */
    unsigned last_err, chip_id, frame_ms;
    int int_level, last_x, last_y, last_fingers, last_event;
    uint8_t raw[6];          /* last raw touch packet bytes 0..5 */
    uint8_t scan[8];         /* I2C addresses that ACKed at boot */
    unsigned scan_count;
} touch_diag_t;
extern touch_diag_t touch_diag;

#endif
