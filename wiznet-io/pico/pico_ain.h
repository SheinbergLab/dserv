/*
 * pico_ain.h -- ADS1115 16-bit I2C ADC: two single-ended "quality" analog inputs
 * for the box, bypassing the RP2350 ADC (and its E9 erratum). Wire an analog
 * joystick's two pots, or any external analog source, to AIN0/AIN1.
 *
 * NON-BLOCKING single-shot state machine: it starts a conversion and only comes
 * back to read it once the conversion is due (timer-gated), so it NEVER stalls
 * the superloop waiting on the ADC -- DI/button latency is unaffected. Each pass
 * does at most one short I2C transaction.
 *
 * Board-adaptive: uses the SDK's default I2C (i2c_default + PICO_DEFAULT_I2C_SDA/
 * SCL_PIN) -> GP4/5 on the W6300 pico2 board, GP6/7 on the Thing Plus. Compiled
 * only when BOX_AIN_ADS1115 is defined; runtime-probes so it's a no-op if absent.
 *
 * Rate + range are live-tunable (config/ain/rate, config/ain/gain): they're
 * written into the ADS1115 config register on every conversion, so a change
 * takes effect on the very next sample -- no reboot.
 */
#ifndef PICO_AIN_H
#define PICO_AIN_H

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "dserv_config.h"
#include <stdio.h>

#define ADS1115_ADDR   0x48
#define AIN_NCH        2         /* AIN0, AIN1 (single-ended)               */
#define AIN_DEADBAND   16        /* publish only when a channel moves > this */

/* config rate field: 0 = default(128 SPS); 1..8 = 8/16/32/64/128/250/475/860 */
static const uint16_t ain_dr_bits[9] = { 0x0080, 0x0000, 0x0020, 0x0040, 0x0060, 0x0080, 0x00A0, 0x00C0, 0x00E0 };
static const uint16_t ain_dr_sps [9] = {   128,      8,     16,     32,     64,    128,    250,    475,    860 };
/* config gain field: 0 = default(+/-4.096V); 1..6 = 6.144/4.096/2.048/1.024/0.512/0.256V FSR */
static const uint16_t ain_pga_bits[7] = { 0x0200, 0x0000, 0x0200, 0x0400, 0x0600, 0x0800, 0x0A00 };

static int             ain_present;
static int16_t         ain_val[AIN_NCH];
static uint8_t         ain_ch, ain_busy;
static absolute_time_t ain_ready;

static inline int ain_wr(uint16_t cfg)
{
    uint8_t b[3] = { 0x01, (uint8_t)(cfg >> 8), (uint8_t) cfg };   /* reg 0x01 = config */
    return i2c_write_timeout_us(i2c_default, ADS1115_ADDR, b, 3, false, 3000) == 3 ? 0 : -1;
}
static inline int ain_rd(uint8_t reg, uint16_t *out)
{
    uint8_t r = reg, b[2];
    if (i2c_write_timeout_us(i2c_default, ADS1115_ADDR, &r, 1, true, 3000) != 1) return -1;
    if (i2c_read_timeout_us (i2c_default, ADS1115_ADDR, b, 2, false, 3000) != 2) return -1;
    *out = ((uint16_t) b[0] << 8) | b[1];
    return 0;
}

static inline void ain_init(void)
{
    i2c_init(i2c_default, 400 * 1000);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);
    uint16_t cfg;
    ain_present = (ain_rd(0x01, &cfg) == 0);
    printf("ain: ADS1115 %s (I2C%d SDA%d SCL%d)\n", ain_present ? "found" : "absent",
           i2c_hw_index(i2c_default), PICO_DEFAULT_I2C_SDA_PIN, PICO_DEFAULT_I2C_SCL_PIN);
}

/* Advance the acquisition state machine (call every superloop pass). Returns a
 * bitmask of channels that just produced a fresh sample (0 = none this call). */
static inline int ain_service(const pico_config_t *c)
{
    if (!ain_present) return 0;
    int ri = c->ain_rate <= 8 ? c->ain_rate : 0;
    if (!ain_busy) {
        int gi = c->ain_gain <= 6 ? c->ain_gain : 0;
        uint16_t cfg = 0x8000                       /* OS: start single conversion */
                     | (0x4000 | (ain_ch << 12))    /* MUX: AINx vs GND             */
                     | ain_pga_bits[gi]             /* PGA (range)                  */
                     | 0x0100                       /* MODE: single-shot            */
                     | ain_dr_bits[ri]              /* data rate                    */
                     | 0x0003;                      /* comparator disabled          */
        if (ain_wr(cfg) == 0) {
            ain_ready = make_timeout_time_us(1000000u / ain_dr_sps[ri] + 200);
            ain_busy = 1;
        }
        return 0;
    }
    if (absolute_time_diff_us(get_absolute_time(), ain_ready) > 0) return 0;   /* not due yet */
    uint16_t v;
    if (ain_rd(0x00, &v) == 0) ain_val[ain_ch] = (int16_t) v;                  /* conv register */
    int done = ain_ch;
    ain_busy = 0;
    ain_ch = (ain_ch + 1) % AIN_NCH;                                           /* next channel */
    return 1 << done;
}

static inline int16_t ain_get(int ch) { return (ch >= 0 && ch < AIN_NCH) ? ain_val[ch] : 0; }

#endif /* PICO_AIN_H */
