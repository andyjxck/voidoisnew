/* PineTime display: ST7789 over SPIM0. LVGL draws into its own buffers
   and hands us finished pixel rectangles via display_send(). */
#include <string.h>
#include "voidos/hal.h"
#include "board.h"

/* nRF52832 SPIM MAXCNT is 8 bits: at most 255 bytes per DMA transfer */
#define SPI_CHUNK 240
static uint8_t dma_buf[SPI_CHUNK] __attribute__((aligned(4)));

static void spi_tx_raw(const uint8_t *p, unsigned len)
{
    while (len) {
        unsigned n = len > SPI_CHUNK ? SPI_CHUNK : len;
        memcpy(dma_buf, p, n);
        SPIM0_EVENTS_END = 0;
        SPIM0_TXD_PTR = (uint32_t)dma_buf;
        SPIM0_TXD_MAXCNT = n;
        SPIM0_RXD_PTR = 0;
        SPIM0_RXD_MAXCNT = 0;
        SPIM0_START = 1;
        for (unsigned spin = 0; !SPIM0_EVENTS_END; spin++) {
            wdt_kick();
            if (spin > 4000000)
                fault_blink(3);
        }
        p += n;
        len -= n;
    }
}

/* The LCD shares SPIM0 with the SPI flash. CS must be released after
   every LCD transaction or flash traffic (WREN/erase/program/RDSR)
   is clocked into the panel as commands. */
static void lcd_select(int on)
{
    if (on)
        GPIO_OUTCLR = BIT(PIN_LCD_CS);
    else
        GPIO_OUTSET = BIT(PIN_LCD_CS);
}

static void lcd_cmd(uint8_t cmd)
{
    GPIO_OUTCLR = BIT(PIN_LCD_DC);
    spi_tx_raw(&cmd, 1);
}

static void lcd_data(const uint8_t *buf, unsigned len)
{
    GPIO_OUTSET = BIT(PIN_LCD_DC);
    spi_tx_raw(buf, len);
}

static void lcd_byte(uint8_t b) { lcd_data(&b, 1); }

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t row[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd(0x2A); lcd_data(col, 4);
    lcd_cmd(0x2B); lcd_data(row, 4);
    lcd_cmd(0x2C);
}

void display_init(void)
{
    GPIO_DIRSET = BIT(PIN_SCK) | BIT(PIN_MOSI) | BIT(PIN_LCD_DC) |
                  BIT(PIN_LCD_CS) | BIT(PIN_LCD_RST) | BIT(PIN_FLASH_CS) |
                  BIT(PIN_BL_L) | BIT(PIN_BL_M) | BIT(PIN_BL_H);
    GPIO_OUTSET = BIT(PIN_LCD_CS) | BIT(PIN_LCD_RST) | BIT(PIN_FLASH_CS);
    GPIO_OUTSET = BIT(PIN_BL_L) | BIT(PIN_BL_M) | BIT(PIN_BL_H); /* off */

    SPIM0_ENABLE = 0;
    SPIM0_PSEL_SCK = PIN_SCK;
    SPIM0_PSEL_MOSI = PIN_MOSI;
    SPIM0_PSEL_MISO = 0xFFFFFFFF;
    SPIM0_FREQ = 0x80000000;   /* 8 MHz */
    SPIM0_CONFIG = 0x6;        /* mode 3, MSB first */
    SPIM0_ENABLE = 7;

    GPIO_OUTCLR = BIT(PIN_LCD_RST);
    hal_delay_ms(10);
    GPIO_OUTSET = BIT(PIN_LCD_RST);
    hal_delay_ms(120);

    lcd_select(1);
    lcd_cmd(0x11); hal_delay_ms(120);  /* SLPOUT */
    lcd_cmd(0x3A); lcd_byte(0x55);     /* 16-bit colour */
    lcd_cmd(0x36); lcd_byte(0x00);     /* MADCTL */
    lcd_cmd(0x21);                     /* INVON */
    lcd_cmd(0x13);                     /* NORON */
    /* GRAM is random out of reset — clear to black BEFORE DISPON or
       the first thing on the glass is noise stripes */
    lcd_cmd(0x2A); lcd_data((uint8_t[]){0, 0, 0, 239}, 4);
    lcd_cmd(0x2B); lcd_data((uint8_t[]){0, 0, 0, 239}, 4);
    lcd_cmd(0x2C);
    GPIO_OUTSET = BIT(PIN_LCD_DC);
    {
        static const uint8_t zeros[240] = {0};
        for (int row = 0; row < 240; row++)
            spi_tx_raw(zeros, sizeof zeros);
    }
    lcd_cmd(0x29); hal_delay_ms(20);   /* DISPON */
    lcd_select(0);
}

/* panel sleep: SLPIN cuts the LCD's own draw current while the watch
   is idle; SLPOUT needs 120 ms before pixels are valid again */
void display_sleep(int on)
{
    lcd_select(1);
    if (on) {
        lcd_cmd(0x28);                 /* DISPOFF */
        lcd_cmd(0x10);                 /* SLPIN */
    } else {
        lcd_cmd(0x11);                 /* SLPOUT */
        hal_delay_ms(120);
        lcd_cmd(0x29);                 /* DISPON */
    }
    lcd_select(0);
}

/* solid fill — used to probe the panel's real RAM extents */
void display_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    lcd_select(1);
    lcd_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
    GPIO_OUTSET = BIT(PIN_LCD_DC);
    unsigned left = (unsigned)(x1 - x0 + 1) * (unsigned)(y1 - y0 + 1);
    while (left) {
        unsigned n = left > SPI_CHUNK / 2 ? SPI_CHUNK / 2 : left;
        for (unsigned i = 0; i < n; i++) {
            dma_buf[2 * i] = (uint8_t)(c >> 8);
            dma_buf[2 * i + 1] = (uint8_t)c;
        }
        SPIM0_EVENTS_END = 0;
        SPIM0_TXD_PTR = (uint32_t)dma_buf;
        SPIM0_TXD_MAXCNT = n * 2;
        SPIM0_START = 1;
        for (unsigned spin = 0; !SPIM0_EVENTS_END; spin++)
            if (spin > 4000000) {
                SPIM0_STOP = 1;
                lcd_select(0);
                return;
            }
        left -= n;
    }
    lcd_select(0);
}

void display_backlight(int level) /* 0..3 */
{
    uint32_t all = BIT(PIN_BL_L) | BIT(PIN_BL_M) | BIT(PIN_BL_H);
    GPIO_OUTSET = all;
    if (level >= 1) GPIO_OUTCLR = BIT(PIN_BL_L);
    if (level >= 2) GPIO_OUTCLR = BIT(PIN_BL_M);
    if (level >= 3) GPIO_OUTCLR = BIT(PIN_BL_H);
}

/* LVGL flush: px is RGB565 already byte-swapped by LVGL
   (LV_COLOR_FORMAT_RGB565_SWAPPED), so it streams straight to the
   panel in <=255-byte DMA chunks — no copy, no per-pixel swap. */
void display_send(int x0, int y0, int x1, int y1, const uint16_t *px)
{
    lcd_select(1);
    lcd_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
    GPIO_OUTSET = BIT(PIN_LCD_DC);
    const uint8_t *p = (const uint8_t *)px;
    unsigned left = (unsigned)(x1 - x0 + 1) * (unsigned)(y1 - y0 + 1) * 2u;
    while (left) {
        unsigned n = left > SPI_CHUNK ? SPI_CHUNK : left;
        SPIM0_EVENTS_END = 0;
        SPIM0_TXD_PTR = (uint32_t)p;
        SPIM0_TXD_MAXCNT = n;
        SPIM0_RXD_MAXCNT = 0;
        SPIM0_START = 1;
        p += n;
        left -= n;
        for (unsigned spin = 0; !SPIM0_EVENTS_END; spin++) {
            wdt_kick();
            if (spin > 4000000)
                fault_blink(3);
        }
    }
    lcd_select(0);
}
