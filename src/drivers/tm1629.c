#include <libopencm3/stm32/gpio.h>
#include "apc_config.h"
#include "tm1629.h"

#define TM1629_CMD_DATA_WRITE_AUTO 0x40U
#define TM1629_CMD_ADDR_BASE       0xC0U
#define TM1629_CMD_DISPLAY_ON_MAX  0x8FU

static const uint8_t k_tm_red_row[8] = {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D};
static const uint8_t k_tm_green_row[8] = {0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F};

static uint8_t s_shadow[16];
static uint8_t s_live[16];
static bool s_pending = false;
static bool s_cooldown_active = false;
static uint32_t s_cooldown_start = 0;
static uint32_t s_cooldown_ticks = 0;

static void tm1629_send_byte(uint8_t data) {
    for (uint8_t bit = 0; bit < 8; bit++) {
        gpio_clear(APC_TM1629_PORT, APC_TM1629_CLK_PIN);
        for (volatile uint8_t d = 0; d < 10; d++) {}

        if ((data & 0x01U) != 0) {
            gpio_set(APC_TM1629_PORT, APC_TM1629_DIO_PIN);
        } else {
            gpio_clear(APC_TM1629_PORT, APC_TM1629_DIO_PIN);
        }
        data >>= 1;

        gpio_set(APC_TM1629_PORT, APC_TM1629_CLK_PIN);
        for (volatile uint8_t d = 0; d < 10; d++) {}
    }
}

void tm1629_init(void) {
    for (uint8_t i = 0; i < 16; i++) {
        s_shadow[i] = 0;
        s_live[i] = 0;
    }
    s_pending = false;
    s_cooldown_active = false;
    s_cooldown_start = 0;
    s_cooldown_ticks = 0;
}

void tm1629_set_pad_rgb(uint8_t row, uint8_t col, bool green, bool red) {
    if (row >= 8 || col >= 8) {
        return;
    }

    uint8_t grn_idx = k_tm_green_row[row];
    uint8_t red_idx = k_tm_red_row[row];
    uint8_t mask = (uint8_t)(1U << col);

    if (green) {
        s_shadow[grn_idx] |= mask;
    } else {
        s_shadow[grn_idx] &= (uint8_t)~mask;
    }

    if (red) {
        s_shadow[red_idx] |= mask;
    } else {
        s_shadow[red_idx] &= (uint8_t)~mask;
    }

    s_pending = true;
}

void tm1629_schedule_flush(uint32_t now_ticks, bool coalesce) {
    s_pending = true;
    if (!coalesce) {
        return;
    }

    if (!s_cooldown_active) {
        s_cooldown_active = true;
        s_cooldown_start = now_ticks;
        s_cooldown_ticks = APC_TM1629_COOLDOWN_IDLE_TICKS;
    } else {
        s_cooldown_start = now_ticks;
        s_cooldown_ticks = APC_TM1629_COOLDOWN_BUSY_TICKS;
    }
}

void tm1629_task(uint32_t now_ticks) {
    if (!s_pending) {
        return;
    }

    if (s_cooldown_active) {
        if ((uint32_t)(now_ticks - s_cooldown_start) < s_cooldown_ticks) {
            return;
        }
        s_cooldown_active = false;
    }
    s_pending = false;

    bool dirty = false;
    for (uint8_t i = 0; i < 16; i++) {
        if (s_shadow[i] != s_live[i]) {
            dirty = true;
            s_live[i] = s_shadow[i];
        }
    }

    if (!dirty) {
        return;
    }

    // Command 1: 0x40 (Data setting: write display, auto-increment address)
    gpio_clear(APC_TM1629_PORT, APC_TM1629_STB_PIN);
    tm1629_send_byte(TM1629_CMD_DATA_WRITE_AUTO);
    gpio_set(APC_TM1629_PORT, APC_TM1629_STB_PIN);
    for (volatile uint8_t d = 0; d < 16; d++) {}

    // Command 2: 0xC0 (Address 0x00) + 16 display bytes
    gpio_clear(APC_TM1629_PORT, APC_TM1629_STB_PIN);
    tm1629_send_byte(TM1629_CMD_ADDR_BASE);
    for (volatile uint8_t d = 0; d < 16; d++) {}
    for (uint8_t i = 0; i < 16; i++) {
        tm1629_send_byte(s_live[i]);
    }
    gpio_set(APC_TM1629_PORT, APC_TM1629_STB_PIN);
    for (volatile uint8_t d = 0; d < 16; d++) {}

    // Command 3: 0x8F (Display ON, max pulse width 14/16)
    gpio_clear(APC_TM1629_PORT, APC_TM1629_STB_PIN);
    tm1629_send_byte(TM1629_CMD_DISPLAY_ON_MAX);
    gpio_set(APC_TM1629_PORT, APC_TM1629_STB_PIN);
}
