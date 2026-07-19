/*
 * pico_mcp3204.h -- MCP3204 12-bit SPI ADC: analog inputs for the box, chiefly a
 * 2-axis analog joystick on the BLE handheld. A passive SAR ADC (unlike the I2C
 * ADS1115 in pico_ain.h): each read is ONE short (~us) SPI transaction -- the
 * conversion happens during the clocking -- so there's no non-blocking state
 * machine to wait on a conversion. Battery-friendly: the MCP3204 self-powers-down
 * between conversions (~uA), so at a modest sample rate its average draw is
 * essentially the conversion energy (vs an always-on I2C coprocessor).
 *
 * Bus: SPI0 on the SDK default pins -- Thing Plus SCK=GP2 / TX(MOSI)=GP3 /
 * RX(MISO)=GP4 -- plus CS on GP5 (the default SPI0 CSn). These are the
 * "default SPI0 / microSD" pins (PINMAP): free on a handheld with no SD card.
 * Reserved from user pin-config in pico_gpio.h while mcp_en. SPI mode 0,0.
 *
 * Samples publish to state/ain/<ch> via the SAME queue/deadband/stamp path as the
 * ADS1115 (publish_ain), so downstream consumers (the ess joystick/analog API)
 * are ADC-agnostic. Wire the joystick pots to CH0 (X) and CH1 (Y). Enable one
 * analog source at a time (mcp_en OR ain_en); each is off by default.
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

/* Overridable per board. Defaults are the SDK's SPI0 pins + GP5 CS. */
#ifndef MCP3204_SPI
#define MCP3204_SPI      spi0
#define MCP3204_PIN_SCK  PICO_DEFAULT_SPI_SCK_PIN   /* 2  */
#define MCP3204_PIN_TX   PICO_DEFAULT_SPI_TX_PIN    /* 3  (MOSI -> MCP DIN) */
#define MCP3204_PIN_RX   PICO_DEFAULT_SPI_RX_PIN    /* 4  (MISO <- MCP DOUT) */
#define MCP3204_PIN_CS   PICO_DEFAULT_SPI_CSN_PIN   /* 5  */
#endif
#define MCP3204_BAUD     (1000 * 1000)   /* 1 MHz: safe at 3.3V (2.7V spec floor is ~1MHz) */
#define MCP3204_NCH      2               /* CH0 = X, CH1 = Y                               */
#define MCP3204_DEADBAND 8               /* 12-bit: publish only when a channel moves > this */

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

static inline uint16_t mcp3204_get(int ch) { return (ch >= 0 && ch < MCP3204_NCH) ? mcp_val[ch] : 0; }

#endif /* PICO_MCP3204_H */
