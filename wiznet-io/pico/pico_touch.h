/* pico_touch.h -- 4D Systems gen4-RP2350 capacitive touch, FocalTech-class on I2C.
 *
 * Same discipline as pico_panel.h: identify the part off the chip before
 * driving it. The board header says LCD_TOUCH_CTP_FT (FocalTech family) but not
 * WHICH one, and the FT5x06 / FT6x06 parts differ in max points and in what
 * their ID registers return.
 *
 * Pins from boards/gen4_rp2350_32ct.h:
 *
 *   LCD_TOUCH_I2C  i2c1     LCD_TOUCH_SCL 39   LCD_TOUCH_SDA 46
 *   LCD_TOUCH_INT  38       LCD_TOUCH_RST 47   LCD_TOUCH_POINTS 5
 *
 * Both bus pins are legal for i2c1 on RP2350 (SDA needs pin%4==2 -> 46, SCL
 * needs pin%4==3 -> 39). Note GP38 would ALSO be a legal i2c1 SDA; it is the
 * INT line here, so it must be left as plain GPIO -- setting GPIO_FUNC_I2C on
 * it would quietly wire a second SDA onto the bus.
 *
 * INT is active-low and asserts while a touch is present. We poll instead of
 * using the IRQ: this lives on core 0 next to the display service, and a 10 ms
 * poll is far below human touch dynamics. IRQ-driving it would only matter if
 * panel taps ever needed real latency -- and PANEL.md is explicit that they
 * must NOT be read as behavioural timestamps.
 */
#ifndef PICO_TOUCH_H
#define PICO_TOUCH_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define TOUCH_I2C   LCD_TOUCH_I2C
#define TOUCH_ADDR  0x38            /* FocalTech CTP default; touch_scan confirms */

/* FocalTech register map (FT5x06 / FT6x06 share this layout) */
#define FT_TD_STATUS 0x02           /* low nibble = points currently down */
#define FT_P1_XH     0x03           /* per-point block, stride 6 */
#define FT_ID_GLIB   0xA1
#define FT_CHIPID    0xA3
#define FT_FIRMID    0xA6
#define FT_VENDID    0xA8

static uint8_t touch_addr = TOUCH_ADDR;

static void touch_bus_init(void)
{
    i2c_init(TOUCH_I2C, 400 * 1000);
    gpio_set_function(LCD_TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(LCD_TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(LCD_TOUCH_SDA);
    gpio_pull_up(LCD_TOUCH_SCL);

    gpio_init(LCD_TOUCH_INT);
    gpio_set_dir(LCD_TOUCH_INT, GPIO_IN);
    gpio_pull_up(LCD_TOUCH_INT);

    gpio_init(LCD_TOUCH_RST);
    gpio_set_dir(LCD_TOUCH_RST, GPIO_OUT);
    gpio_put(LCD_TOUCH_RST, 0); sleep_ms(10);
    gpio_put(LCD_TOUCH_RST, 1); sleep_ms(300);   /* FocalTech boot is slow (~300 ms) */
}

static int touch_rd(uint8_t reg, uint8_t *buf, int n)
{
    if (i2c_write_blocking(TOUCH_I2C, touch_addr, &reg, 1, true) < 0) return -1;
    return i2c_read_blocking(TOUCH_I2C, touch_addr, buf, (size_t) n, false);
}

/* Walk the whole 7-bit space rather than assuming 0x38. If the module ever
 * ships a different CTP, a scan says so immediately; a failed read at a
 * hardcoded address looks identical to a dead bus. */
static int touch_scan(char *out, int outsz)
{
    int n = 0, found = 0;
    n += snprintf(out + n, (size_t) (outsz - n), "  i2c1 scan:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        uint8_t b;
        if (i2c_read_blocking(TOUCH_I2C, a, &b, 1, false) >= 0) {
            n += snprintf(out + n, (size_t) (outsz - n), " 0x%02X", a);
            if (!found) { touch_addr = a; found = 1; }
        }
        if (n >= outsz - 32) break;
    }
    snprintf(out + n, (size_t) (outsz - n), found ? "\r\n" : " (nothing responded)\r\n");
    return found;
}

static void touch_probe(char *out, int outsz)
{
    uint8_t chip = 0, vend = 0, firm = 0, glib = 0, td = 0;
    int n;

    touch_bus_init();
    out[0] = 0;                          /* touch_scan writes from the start */
    if (!touch_scan(out, outsz)) return; /* nothing on the bus: say so, read nothing */
    n = (int) strlen(out);

    touch_rd(FT_CHIPID, &chip, 1);
    touch_rd(FT_VENDID, &vend, 1);
    touch_rd(FT_FIRMID, &firm, 1);
    touch_rd(FT_ID_GLIB, &glib, 1);
    touch_rd(FT_TD_STATUS, &td, 1);

    n += snprintf(out + n, (size_t) (outsz - n),
                  "  addr 0x%02X  CHIPID 0x%02X  VENDID 0x%02X  FIRMID 0x%02X  GLIB 0x%02X\r\n"
                  "  TD_STATUS 0x%02X (%d down)  INT=%d\r\n"
                  "  known CHIPIDs: 06=FT6206 36=FT6236 55=FT5306 0A=FT5316 64=FT6336 54=FT5x46\r\n",
                  touch_addr, chip, vend, firm, glib,
                  td, td & 0x0F, gpio_get(LCD_TOUCH_INT));

    /* Raw low block. Distinguishes "TD_STATUS is genuinely 0xFF" from "the whole
     * data page reads 0xFF", which are different faults: the first is a mode/
     * register-map problem, the second is the bus NAKing and floating high --
     * and the ID registers reading fine already rules the bus out. */
    {
        uint8_t b[16];
        int rc = touch_rd(0x00, b, (int) sizeof b);
        n += snprintf(out + n, (size_t) (outsz - n), "  regs 00-0F (rc=%d):", rc);
        if (rc >= 0)
            for (unsigned i = 0; i < sizeof b; i++)
                n += snprintf(out + n, (size_t) (outsz - n), " %02X", b[i]);
        snprintf(out + n, (size_t) (outsz - n), "\r\n");
    }
}

/* Raw first-16 dump for bring-up: shows what the controller reports without
 * any register-map assumption of ours in the way. */
static int touch_raw(uint8_t *b16) { return touch_rd(0x00, b16, 16); }

/* ---- point reading ---- */

typedef struct { uint16_t x, y; uint8_t id, ev; } touch_pt_t;

/* One block read of the status byte plus every point slot -- FocalTech latches
 * the whole set, so reading it piecemeal can tear across a lift/press. */
static int touch_read(touch_pt_t *pt, int max)
{
    uint8_t b[1 + 6 * LCD_TOUCH_POINTS];
    int nd;

    if (touch_rd(FT_TD_STATUS, b, (int) sizeof b) < 0) return -1;
    nd = b[0] & 0x0F;
    /* REJECT an implausible count, do not clamp it. Idle, this part returns
     * 0xFF across the whole data page (-> 15 points); clamping that to 5 would
     * manufacture five phantom touches every poll, which is exactly the
     * "fields that report memory not reality" trap. */
    if (nd > LCD_TOUCH_POINTS) return -2;
    if (nd > max) nd = max;

    for (int i = 0; i < nd; i++) {
        const uint8_t *p = b + 1 + 6 * i;
        pt[i].ev = (uint8_t) (p[0] >> 6);                       /* 0 down 1 up 2 contact */
        pt[i].x  = (uint16_t) (((p[0] & 0x0F) << 8) | p[1]);
        pt[i].y  = (uint16_t) (((p[2] & 0x0F) << 8) | p[3]);
        pt[i].id = (uint8_t) (p[2] >> 4);
#ifdef LCD_TOUCH_MIRROR_Y
        /* MEASURED, not assumed (`panel corner`, 2026-08-05): touching the
         * displayed top-left corner reports y=289 and the bottom-left reports
         * y=27, while x tracks correctly in both. So the controller's Y runs
         * opposite the display's under our MADCTL, exactly as the board header
         * advertises. Driven off the header symbol rather than hardcoded, so
         * the other gen4 sizes pick up their own convention. */
        if (pt[i].y < LCD_HEIGHT) pt[i].y = (uint16_t) (LCD_HEIGHT - 1 - pt[i].y);
#endif
    }
    return nd;
}

#endif /* PICO_TOUCH_H */
