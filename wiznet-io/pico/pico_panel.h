/* pico_panel.h -- 4D Systems gen4-RP2350 TFT panel, MCU-16 (8080) bus.
 *
 * Stage 0: bit-banged bus + a controller-ID probe. 4D do not publish which
 * driver IC is behind the bus (Graphics4D abstracts it, and the resource-centre
 * page links only the RP2350 datasheet), so we ask the chip directly rather
 * than guess an init sequence -- a wrong init is a blank screen with no
 * diagnostic, whereas a wrong ID read is obvious. The RD line exists, so this
 * is what it is for. The fast PIO+DMA write path lands on top of this once the
 * part is known; the pin plumbing here does not change.
 *
 * Pins come from the SDK board header (boards/gen4_rp2350_32ct.h), NOT from
 * pico_gpio.h's configurable set:
 *
 *   LCD_BACKLIGHT 17   LCD_RS_PIN 18 (0=command, 1=data)
 *   LCD_WR_PIN    19   LCD_RD_PIN 20
 *   LCD_DATA0_PIN 21   -> D0..D15 = GP21..GP36
 *   LCD_RESET     37
 *
 * There is NO chip-select pin on the module -- CS is tied asserted, so a bus
 * cycle is just RS + a WR (or RD) strobe.
 *
 * THE DATA BUS STRADDLES THE SIO BANK BOUNDARY: D0..D10 are GP21..GP31 (low
 * bank) and D11..D15 are GP32..GP36 (high bank). Every access therefore uses
 * the *64* GPIO API (gpio_put_masked64 / gpio_get_all64 / gpio_set_dir_*64),
 * which RP2350B has and which makes the split invisible. Using the 32-bit
 * calls here would silently drive only the low 11 bits. This is also why 4D
 * drive it from PIO: with GPIOBASE=16 the 16 pins are one contiguous field.
 */
#ifndef PICO_PANEL_H
#define PICO_PANEL_H

#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define PANEL_D0     LCD_DATA0_PIN
#define PANEL_DMASK  (0xFFFFULL << PANEL_D0)      /* D0..D15, crosses GP31/GP32 */

/* ---- raw bus ---- */

static inline void panel_bus_out(void) { gpio_set_dir_out_masked64(PANEL_DMASK); }
static inline void panel_bus_in(void)  { gpio_set_dir_in_masked64(PANEL_DMASK); }

static inline void panel_put(uint16_t v)
{
    gpio_put_masked64(PANEL_DMASK, ((uint64_t) v) << PANEL_D0);
}

static inline uint16_t panel_get(void)
{
    return (uint16_t) ((gpio_get_all64() >> PANEL_D0) & 0xFFFFu);
}

/* WR is edge-triggered: the controller latches on the RISING edge. The nops
 * hold each half-cycle well past the ~15 ns minimum at 150 MHz -- this is the
 * probe path, not the pixel path, so correctness over speed. */
static inline void panel_wr(void)
{
    gpio_put(LCD_WR_PIN, 0);
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop");
    gpio_put(LCD_WR_PIN, 1);
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop");
}

static inline void panel_cmd(uint16_t c)
{
    gpio_put(LCD_RS_PIN, 0); panel_put(c); panel_wr();
}

static inline void panel_dat(uint16_t d)
{
    gpio_put(LCD_RS_PIN, 1); panel_put(d); panel_wr();
}

/* Register reads are MUCH slower than writes on every controller in this
 * family (ILI9341 wants RD low >= 355 ns vs a 66 ns write cycle), and reading
 * too fast is the classic way to get plausible-looking garbage. 2 us of margin
 * costs nothing here. */
static uint16_t panel_rd(void)
{
    uint16_t v;
    gpio_put(LCD_RS_PIN, 1);
    panel_bus_in();
    gpio_put(LCD_RD_PIN, 0);
    busy_wait_us_32(2);
    v = panel_get();
    gpio_put(LCD_RD_PIN, 1);
    busy_wait_us_32(2);
    panel_bus_out();
    return v;
}

/* ---- bring-up ---- */

static void panel_bus_init(void)
{
    static const uint8_t ctl[] = { LCD_RS_PIN, LCD_WR_PIN, LCD_RD_PIN, LCD_RESET, LCD_BACKLIGHT };
    for (unsigned i = 0; i < sizeof ctl; i++) {
        gpio_init(ctl[i]);
        gpio_set_dir(ctl[i], GPIO_OUT);
    }
    gpio_put(LCD_RS_PIN, 1);
    gpio_put(LCD_WR_PIN, 1);          /* strobes idle high */
    gpio_put(LCD_RD_PIN, 1);
    gpio_put(LCD_BACKLIGHT, 0);       /* stay dark until there is something to show */
    gpio_put(LCD_RESET, 1);

    for (unsigned i = 0; i < 16; i++) gpio_init(PANEL_D0 + i);
    panel_bus_out();
    panel_put(0);

    gpio_put(LCD_RESET, 0); sleep_ms(20);      /* >= 10 us low is spec; 20 ms is free */
    gpio_put(LCD_RESET, 1); sleep_ms(150);     /* controllers want ~120 ms before commands */
}

/* Probe: dump raw words from every ID register this controller family might
 * answer on. We deliberately do NOT decode here -- the whole point is that the
 * part is unknown, so print what came back and let a human match it:
 *
 *   0x04 RDDID   -> dummy, mfg, version, driver
 *   0xD3 RDID4   -> dummy, 0x00, 0x93, 0x41   (ILI9341)  / 0x94,0x88 (ILI9488)
 *   0xDA/DB/DC   -> ID1 / ID2 / ID3, one byte each
 *   0x09 RDDST   -> 4 status bytes
 *
 * Byte lane matters and varies by part in MCU-16: some return the value on
 * D0..D7, some on D8..D15. Printing the full 16-bit word means we can see
 * which, instead of guessing and masking the evidence away.
 */
static void panel_probe(char *out, int outsz)
{
    static const uint8_t regs[] = { 0x04, 0xD3, 0xDA, 0xDB, 0xDC, 0x09 };
    int n = 0;

    panel_bus_init();

    for (unsigned r = 0; r < sizeof regs; r++) {
        uint16_t w[5];
        panel_cmd(regs[r]);
        for (unsigned i = 0; i < 5; i++) w[i] = panel_rd();
        n += snprintf(out + n, (size_t) (outsz - n),
                      "  %02X: %04X %04X %04X %04X %04X\r\n",
                      regs[r], w[0], w[1], w[2], w[3], w[4]);
        if (n >= outsz - 64) break;
    }

    /* Backlight on as the one checkpoint visible without a working driver: if
     * the panel lights (blank/noise is fine) then GP17 and the module's power
     * rails are good, which separates "bus wrong" from "board wrong". */
    gpio_put(LCD_BACKLIGHT, 1);
    snprintf(out + n, (size_t) (outsz - n), "  backlight ON (expect a lit, blank screen)\r\n");
}

/* ---- ILI9341 ----
 *
 * IDENTIFIED ON HARDWARE 2026-08-05, not assumed: `panel id` read 0xD3 (RDID4)
 * and got `0000 0000 0093 0041` -- dummy, 0x00, 0x93, 0x41. The same probe
 * settled the byte lane: 8-bit values come back on D0..D7 (0x0093), NOT the
 * high lane, so command parameters go out in the low byte and only RAMWR
 * pixel data uses all 16 lines.
 */
#define ILI_SWRESET 0x01
#define ILI_SLPOUT  0x11
#define ILI_DISPON  0x29
#define ILI_CASET   0x2A
#define ILI_PASET   0x2B
#define ILI_RAMWR   0x2C
#define ILI_MADCTL  0x36
#define ILI_PIXFMT  0x3A

/* MADCTL 0x48 = MX | BGR -> native portrait 240x320, matching the board
 * header's LCD_ORIENTATION PORTRAIT / LCD_WIDTH 240 / LCD_HEIGHT 320. */
#define ILI_MADCTL_PORTRAIT 0x48

static void panel_init(void)
{
    /* {cmd, nargs, args...} run in order; 0xFF = delay-ms pseudo-op. Power/timing
     * block is the ILI9341 vendor sequence -- undocumented registers, copied as a
     * unit; do not reorder or "clean up". */
    static const uint8_t seq[] = {
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x23,                       /* PWCTR1  */
        0xC1, 1, 0x10,                       /* PWCTR2  */
        0xC5, 2, 0x3E, 0x28,                 /* VMCTR1  */
        0xC7, 1, 0x86,                       /* VMCTR2  */
        ILI_MADCTL, 1, ILI_MADCTL_PORTRAIT,
        ILI_PIXFMT, 1, 0x55,                 /* 16 bpp RGB565 */
        0xB1, 2, 0x00, 0x18,                 /* frame rate ~70 Hz */
        0xB6, 3, 0x08, 0x82, 0x27,           /* display function */
        0xF2, 1, 0x00,                       /* 3Gamma off */
        0x26, 1, 0x01,                       /* gamma curve 1 */
        0xE0, 15, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00,
        0xE1, 15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F,
        ILI_SLPOUT, 0xFF, 120,
        ILI_DISPON, 0xFF, 20,
        0x00
    };
    const uint8_t *p = seq;

    panel_bus_init();
    panel_cmd(ILI_SWRESET);
    sleep_ms(150);

    while (*p) {
        uint8_t cmd = *p++, n = *p++;
        panel_cmd(cmd);
        if (n == 0xFF) { sleep_ms(*p++); continue; }
        while (n--) panel_dat(*p++);
    }
    gpio_put(LCD_BACKLIGHT, 1);
}

static void panel_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    panel_cmd(ILI_CASET);
    panel_dat(x0 >> 8); panel_dat(x0 & 0xFF); panel_dat(x1 >> 8); panel_dat(x1 & 0xFF);
    panel_cmd(ILI_PASET);
    panel_dat(y0 >> 8); panel_dat(y0 & 0xFF); panel_dat(y1 >> 8); panel_dat(y1 & 0xFF);
    panel_cmd(ILI_RAMWR);
}

/* Bit-banged fill. The bus value is constant across the run, so we park the
 * data once and only strobe WR -- ~1 GPIO write per pixel instead of 2, which
 * is what makes a full-screen clear tolerable (~8 ms) before the PIO+DMA path
 * exists. Stage 1 replaces the inner loop, not the ILI9341 sequence above. */
static void panel_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565)
{
    uint32_t n = (uint32_t) w * h;
    if (!w || !h) return;
    panel_window(x, y, x + w - 1, y + h - 1);
    gpio_put(LCD_RS_PIN, 1);
    panel_put(rgb565);
    while (n--) panel_wr();
}

static inline void panel_fill(uint16_t rgb565)
{
    panel_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, rgb565);
}

/* Arbitrary pixels: unlike panel_fill_rect this cannot park the bus, so it pays
 * a bus write AND a strobe per pixel. This is the honest upper bound for what a
 * real repaint costs, and the number that decides whether PIO+DMA is worth it. */
static void panel_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px)
{
    uint32_t n = (uint32_t) w * h;
    if (!w || !h) return;
    panel_window(x, y, x + w - 1, y + h - 1);
    gpio_put(LCD_RS_PIN, 1);
    while (n--) { panel_put(*px++); panel_wr(); }
}

#define RGB565(r, g, b) ((uint16_t) (((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))

/* Read back what init WROTE. This is the verification that does not need eyes
 * on the glass: MADCTL and COLMOD are read-write, so if 0x0B returns the 0x48
 * we sent and 0x0C returns 0x55, then command+parameter writes are landing
 * correctly on the right byte lane -- a lit screen showing the wrong thing and
 * a dead bus look identical from here otherwise. 0x0A decodes the power state
 * so "init ran but the panel is still asleep" is distinguishable too. */
static void panel_status(char *out, int outsz)
{
    uint16_t pm, madctl, colmod;

    panel_cmd(0x0A); panel_rd(); pm     = panel_rd();   /* RDDPM      (1 dummy) */
    panel_cmd(0x0B); panel_rd(); madctl = panel_rd();   /* RDDMADCTL  (1 dummy) */
    panel_cmd(0x0C); panel_rd(); colmod = panel_rd();   /* RDDCOLMOD  (1 dummy) */

    snprintf(out, (size_t) outsz,
             "  RDDPM    0x%02X  booster=%d sleep_out=%d display_on=%d\r\n"
             "  RDDMADCTL 0x%02X (wrote 0x%02X)%s\r\n"
             "  RDDCOLMOD 0x%02X  mcu_ifc=%d%s\r\n",
             pm & 0xFF, !!(pm & 0x80), !!(pm & 0x10), !!(pm & 0x04),
             madctl & 0xFF, ILI_MADCTL_PORTRAIT,
             (madctl & 0xFF) == ILI_MADCTL_PORTRAIT ? "  OK" : "  MISMATCH",
             colmod & 0xFF, colmod & 0x07,
             /* Compare ONLY bits [2:0]. RDDCOLMOD splits the field: [6:4] is the
              * RGB (DPI) interface format and [2:0] the MCU interface. We write
              * 0x55 to COLMOD but read 0x05 back, because this module has no DPI
              * video interface wired -- so the RGB nibble is legitimately 0 and
              * only the MCU nibble echoes our 16 bpp. Checking the whole byte
              * reports a failure that is not one. */
             (colmod & 0x07) == 0x05 ? "  OK (16 bpp; RGB nibble unused on this module)"
                                     : "  MISMATCH");
}

/* Colour bars: proves geometry AND channel order in one glance. If the top bar
 * reads blue instead of red, the BGR bit in MADCTL is inverted for this panel. */
static void panel_test_pattern(void)
{
    static const uint16_t bar[] = {
        RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255),
        RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 0, 0),
    };
    unsigned nbar = sizeof bar / sizeof bar[0];
    uint16_t bh = LCD_HEIGHT / nbar;
    for (unsigned i = 0; i < nbar; i++)
        panel_fill_rect(0, (uint16_t) (i * bh), LCD_WIDTH,
                        (uint16_t) (i == nbar - 1 ? LCD_HEIGHT - i * bh : bh), bar[i]);
}

#endif /* PICO_PANEL_H */
