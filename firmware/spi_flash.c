/* External SPI NOR flash (XT25F08B-compatible JEDEC part) on the shared
   SPIM0 bus, CS on PIN_FLASH_CS. Used by the BLE DFU service to store the
   secondary-slot image at offset 0x40000. */
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "voidos/hal.h"

static uint8_t fx_buf[264] __attribute__((aligned(4)));

static void flash_sel(int on)
{
    if (on)
        GPIO_OUTCLR = BIT(PIN_FLASH_CS);
    else
        GPIO_OUTSET = BIT(PIN_FLASH_CS);
}

/* bidirectional xfer; tx may be NULL (clocks 0xFF), rx may be NULL */
static void flash_xfer(const uint8_t *tx, uint8_t *rx, unsigned len)
{
    while (len) {
        unsigned n = len > 255 ? 255 : len;
        if (tx) {
            memcpy(fx_buf, tx, n);
            tx += n;
        } else {
            memset(fx_buf, 0xFF, n);
        }
        SPIM0_EVENTS_END = 0;
        SPIM0_TXD_PTR = (uint32_t)fx_buf;
        SPIM0_TXD_MAXCNT = n;
        SPIM0_RXD_PTR = rx ? (uint32_t)fx_buf : 0;
        SPIM0_RXD_MAXCNT = rx ? n : 0;
        SPIM0_START = 1;
        for (unsigned spin = 0; !SPIM0_EVENTS_END; spin++)
            if (spin > 4000000) {
                SPIM0_STOP = 1;
                return;
            }
        if (rx) {
            memcpy(rx, fx_buf, n);
            rx += n;
        }
        len -= n;
    }
}

static void flash_cmd_only(uint8_t cmd)
{
    flash_sel(1);
    flash_xfer(&cmd, 0, 1);
    flash_sel(0);
}

static uint8_t flash_status(void)
{
    uint8_t rx[1];
    flash_sel(1);
    flash_xfer((const uint8_t *)"\x05", 0, 1);
    flash_xfer(0, rx, 1);
    flash_sel(0);
    return rx[0];
}

static void flash_wait(void)
{
    /* WIP bit; kick the watchdog — sector erase can take tens of ms.
       Bounded: a wedged flash must not freeze the watch forever */
    for (unsigned i = 0; flash_status() & 1; i++) {
        if ((i & 0x3FF) == 0)
            wdt_kick();
        if (i > 200000)
            break;                  /* ~2 s worst case: give up */
    }
}

void spi_flash_init(void)
{
    /* display_init left MISO unconnected; attach it for flash reads */
    SPIM0_ENABLE = 0;
    SPIM0_PSEL_MISO = PIN_MISO;
    SPIM0_ENABLE = 7;
    /* release from deep power-down (0xAB + 3 dummy bytes), as InfiniTime's
       SpiNorFlash::Wakeup does at every boot: InfiniTime puts the XT25F32B
       to sleep (0xB9) when its screen goes off, and a reset straight from
       there into this firmware left every read returning junk (settings
       lost, DFU writes silently failing) */
    {
        static const uint8_t rdp[4] = {0xAB, 0, 0, 0};
        uint8_t id;
        flash_sel(1);
        flash_xfer(rdp, 0, 4);
        flash_xfer(0, &id, 1);
        flash_sel(0);
        hal_delay_ms(1);               /* tRES1 is a few us */
    }
    (void)flash_status();
}

void spi_flash_read(uint32_t addr, uint8_t *buf, unsigned len)
{
    uint8_t hdr[4] = {0x03, addr >> 16, addr >> 8, addr};
    flash_sel(1);
    flash_xfer(hdr, 0, 4);
    flash_xfer(0, buf, len);
    flash_sel(0);
}

void spi_flash_write(uint32_t addr, const uint8_t *buf, unsigned len)
{
    while (len) {
        unsigned n = 256 - (addr & 0xFF);
        if (n > len)
            n = len;
        uint8_t hdr[4] = {0x02, addr >> 16, addr >> 8, addr};
        flash_cmd_only(0x06);              /* WREN */
        flash_sel(1);
        flash_xfer(hdr, 0, 4);
        flash_xfer(buf, 0, n);
        flash_sel(0);
        flash_wait();
        addr += n;
        buf += n;
        len -= n;
    }
}

void spi_flash_sector_erase(uint32_t addr)
{
    uint8_t hdr[4] = {0x20, addr >> 16, addr >> 8, addr};
    flash_cmd_only(0x06);
    flash_sel(1);
    flash_xfer(hdr, 0, 4);
    flash_sel(0);
    flash_wait();
}
