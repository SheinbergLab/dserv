/*
 * pico_mcp3204.h -- MCP3204 12-bit SPI ADC: the box's analog input, chiefly a
 * 2-axis analog joystick on the BLE handheld. A passive SAR ADC: each read is
 * ONE short (~us) SPI transaction -- the conversion happens during the clocking
 * -- so there's no non-blocking state machine to wait on a conversion. Battery-
 * friendly: the MCP3204 self-powers-down between conversions (~uA), so at a
 * modest sample rate its average draw is essentially the conversion energy (vs
 * an always-on I2C coprocessor). (Replaced the ADS1115 I2C path, 2026-07-19.)
 *
 * Bus: SPI0 on the bank-0 pins -- SCK=GP2 / TX(MOSI/DIN)=GP3 / RX(MISO/DOUT)=GP4
 * -- plus a software CS on GP5. Pinned to these (not the SDK board default SPI0,
 * which is GP16-19 on PICO_BOARD=pico2 and collides with the W6300 QSPI block on
 * the EVB) so one layout is valid on every board; GP2/3 coincide with the OLED's
 * write-only bus (shared, distinct CS). Pin macros + rationale live in
 * pico_gpio.h; reserved from user pin-config while mcp_en. SPI mode 0,0.
 *
 * All channels are grabbed in one fast scan (mcp3204_scan) and published as a
 * single packed state/ain/scan snapshot (int16[4], one timestamp) -- see
 * mcp_service_core0 / publish_ain_scan. CH0 = joystick X, CH1 = Y; CH2/CH3 free.
 *
 * Always compiled; runtime-enabled via `mcp enable 1` (persisted) + a boot init.
 */
#ifndef PICO_MCP3204_H
#define PICO_MCP3204_H

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "dserv_config.h"
#include <stdio.h>

/* Pins come from pico_gpio.h (MCP3204_PIN_*, GP2/3/4/5) so pico_gpio_reserved()
 * and this driver agree on ONE definition -- see the note there for why 2/3/4/5
 * and not the SDK's board-default SPI0 pins (16-19 collide with the W6300 on the
 * EVB). Fallbacks below cover a translation unit that includes this without
 * pico_gpio.h; still overridable per board at build time. Bus = spi0. */
#ifndef MCP3204_SPI
#define MCP3204_SPI      spi0
#endif
#ifndef MCP3204_PIN_SCK
#define MCP3204_PIN_SCK  2
#endif
#ifndef MCP3204_PIN_TX
#define MCP3204_PIN_TX   3   /* MOSI -> MCP DIN  */
#endif
#ifndef MCP3204_PIN_RX
#define MCP3204_PIN_RX   4   /* MISO <- MCP DOUT */
#endif
#ifndef MCP3204_PIN_CS
#define MCP3204_PIN_CS   5
#endif
#define MCP3204_BAUD     (1000 * 1000)   /* 1 MHz: safe at 3.3V (2.7V spec floor is ~1MHz) */
#define MCP3204_NCH      4               /* all 4 channels grabbed in one fast scan (CH0=X, CH1=Y) */
#define MCP3204_DEADBAND 8               /* 12-bit: publish the snapshot only when a channel moves > this */

static uint16_t mcp_val[MCP3204_NCH];
static uint8_t  mcp_inited;

static inline void mcp3204_init(void)
{
    spi_init(MCP3204_SPI, MCP3204_BAUD);            /* mode 0,0 is spi_init's default */
    gpio_set_function(MCP3204_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(MCP3204_PIN_TX,  GPIO_FUNC_SPI);
    gpio_set_function(MCP3204_PIN_RX,  GPIO_FUNC_SPI);
    gpio_init(MCP3204_PIN_CS);
    gpio_set_dir(MCP3204_PIN_CS, GPIO_OUT);
    gpio_put(MCP3204_PIN_CS, 1);                    /* CS idle high */
    mcp_inited = 1;
    printf("mcp3204: SPI%d SCK%d TX%d RX%d CS%d @ %dkHz\n", spi_get_index(MCP3204_SPI),
           MCP3204_PIN_SCK, MCP3204_PIN_TX, MCP3204_PIN_RX, MCP3204_PIN_CS, MCP3204_BAUD / 1000);
}

/* One 12-bit single-ended conversion (blocking, ~us). 3-byte MCP3204/3208 frame:
 *   tx[0] = 0000 01 SGL D2 ; tx[1] = D1 D0 xxxxxx ; tx[2] = 0
 *   result = ((rx[1] & 0x0F) << 8) | rx[2]      (12 bits, MSB-first)
 * For the MCP3204 (4 channels) D2 is always 0; single-ended (SGL=1). */
static inline uint16_t mcp3204_read(int ch)
{
    uint8_t tx[3] = { (uint8_t)(0x06 | ((ch & 0x04) >> 2)), (uint8_t)((ch & 0x03) << 6), 0 };
    uint8_t rx[3];
    gpio_put(MCP3204_PIN_CS, 0);
    spi_write_read_blocking(MCP3204_SPI, tx, rx, 3);
    gpio_put(MCP3204_PIN_CS, 1);
    return (uint16_t)(((rx[1] & 0x0F) << 8) | rx[2]);
}

/* Fast scan of all 4 channels into out[4] (~4 x us SPI reads). Cheap enough to
 * always grab the full snapshot -- the extra channels ride free in the message. */
static inline void mcp3204_scan(uint16_t out[MCP3204_NCH])
{
    for (int ch = 0; ch < MCP3204_NCH; ch++) out[ch] = mcp3204_read(ch);
}

static inline uint16_t mcp3204_get(int ch) { return (ch >= 0 && ch < MCP3204_NCH) ? mcp_val[ch] : 0; }

#endif /* PICO_MCP3204_H */
