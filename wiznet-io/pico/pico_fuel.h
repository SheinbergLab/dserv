/*
 * pico_fuel.h -- MAX17048 LiPo fuel gauge readout (e.g. SparkFun Thing Plus
 * RP2350, where it sits on the Qwiic I2C bus). Compiled only when
 * BOX_FUEL_MAX17048 is defined; runtime-probes the chip, so it's a safe no-op if
 * the gauge isn't populated. Uses the board's default I2C (i2c_default + the
 * PICO_DEFAULT_I2C_SDA/SCL pins from the SDK board header).
 */
#ifndef PICO_FUEL_H
#define PICO_FUEL_H

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define MAX17048_ADDR   0x36
#define MAX17048_VCELL  0x02
#define MAX17048_SOC    0x04

static int fuel_present;

static inline int fuel_rd16(uint8_t reg, uint16_t *out)
{
    uint8_t r = reg, b[2];
    if (i2c_write_timeout_us(i2c_default, MAX17048_ADDR, &r, 1, true, 2000) != 1) return -1;
    if (i2c_read_timeout_us(i2c_default, MAX17048_ADDR, b, 2, false, 2000) != 2) return -1;
    *out = ((uint16_t) b[0] << 8) | b[1];
    return 0;
}

static inline void fuel_init(void)
{
    i2c_init(i2c_default, 400 * 1000);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);
    uint16_t v;
    fuel_present = (fuel_rd16(MAX17048_VCELL, &v) == 0);
    printf("fuel: MAX17048 %s (I2C%d SDA%d SCL%d)\n", fuel_present ? "found" : "absent",
           i2c_hw_index(i2c_default), PICO_DEFAULT_I2C_SDA_PIN, PICO_DEFAULT_I2C_SCL_PIN);
}

/* battery charge %, 0..100, or -1 if absent/error */
static inline int fuel_soc_pct(void)
{
    uint16_t s;
    if (!fuel_present || fuel_rd16(MAX17048_SOC, &s) != 0) return -1;
    int pct = s >> 8;                       /* SOC LSB = 1/256 % */
    return pct > 100 ? 100 : pct;
}

/* battery millivolts, or -1 if absent/error */
static inline int fuel_mv(void)
{
    uint16_t v;
    if (!fuel_present || fuel_rd16(MAX17048_VCELL, &v) != 0) return -1;
    return (int) v * 5 / 64;                /* VCELL LSB = 78.125 uV = 5/64 mV */
}

#endif /* PICO_FUEL_H */
