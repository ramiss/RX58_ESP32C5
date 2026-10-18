// SPDX-License-Identifier: GPL-3.0-only
#include "rssi_out.h"

#include "driver/ledc.h"

#define DUTY_BITS LEDC_TIMER_10_BIT
#define DUTY_MAX  1023u
#define PWM_HZ    78125u             // 80 MHz / 1024: ripple far above the RC corner

static int32_t s_lo16 = 16 * (-60 + 100), s_hi16 = 16 * (0 + 100);
static uint32_t s_duty;

esp_err_t rssi_out_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;
    const ledc_channel_config_t c = {
        .gpio_num = RSSI_OUT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&c);
}

void rssi_out_range(int lo_dbfs, int hi_dbfs)
{
    if (hi_dbfs <= lo_dbfs) return;
    s_lo16 = 16 * (lo_dbfs + 100);
    s_hi16 = 16 * (hi_dbfs + 100);
}

static bool s_on = true;
static int32_t s_forced = -1;

void rssi_out_force(int32_t duty)
{
    s_forced = duty < 0 ? -1 : duty > (int32_t)DUTY_MAX ? (int32_t)DUTY_MAX : duty;
    if (s_forced >= 0) {
        s_duty = (uint32_t)s_forced;
        (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, s_duty);
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

void rssi_out_enable(bool on)
{
    s_on = on;
    if (!on) rssi_out_set(0);
}

void rssi_out_set(uint16_t rssi16)
{
    if (s_forced >= 0) return;
    int32_t v = rssi16;
    uint32_t duty = !s_on || v <= s_lo16 ? 0u : v >= s_hi16 ? DUTY_MAX
                  : (uint32_t)((v - s_lo16) * (int32_t)DUTY_MAX / (s_hi16 - s_lo16));
    if (duty == s_duty) return;
    s_duty = duty;
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint32_t rssi_out_duty(void) { return s_duty; }
