// SPDX-License-Identifier: GPL-3.0-only
// RX5808 / RTC6715 register file and the EXT register state
// (docs/RX5808-C5_SPI_Interface.md, sections 4 and 7).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define REG_SYNTH_A    0x0
#define REG_SYNTH_B    0x1
#define REG_POWER      0xA
#define REG_EXT        0xE
#define REG_STATE      0xF

#define REGS_IDENTIFY  0xC5009u      // 0xC5 + interface version 0.9 (section 7.3)
#define REGS_MIN_MHZ   5180u
#define REGS_MAX_MHZ   5945u
#define REGS_SLOTS     8

// EXT sub-registers (section 7.2).
#define EXT_MODE          0x0
#define EXT_SLOT1         0x1         // .. 0x8
#define EXT_RSSI_PIN_SLOT 0x9
#define EXT_GAIN          0xA
#define EXT_READ_SELECT   0xB
#define EXT_OPTIONS       0xC
#define EXT_NOP           0xF

#define MODE_RX5808       0
#define MODE_MULTI        1
#define MODE_VIDEO        2

#define OPT_SMOOTHING     0x1         // median of 3, default on

// READ_SELECT value: block 0-3, | READ_REARM = re-arm the last snapshot.
#define READ_REARM        0x80

typedef struct {
    uint8_t mode;                     // requested mode (MODE_*)
    uint16_t slot_mhz[REGS_SLOTS];    // 0 = slot off
    uint8_t pin_slot;                 // 1-8
    uint8_t gain;                     // 0 = firmware default
    uint8_t options;                  // OPT_*
} ext_state_t;

void regs_reset(void);
// Store a write (any context, incl. ISRs). Returns true if it changes what
// the receiver should do (frequency, power, EXT state), see
// regs_take_changes(). EXT writes (address 0x0E) are decoded here; a
// READ_SELECT write calls the select callback (in the caller's context).
bool regs_write(uint32_t addr, uint32_t data);
// The read-back value (datasheet 5.2).
uint32_t regs_read(uint32_t addr);

// Called from regs_write() for READ_SELECT (in the SPI interrupt).
typedef void (*regs_select_cb_t)(uint32_t value);
void regs_set_select_cb(regs_select_cb_t cb);
// esp_timer time of the last MODE write that changed the mode.
int64_t regs_mode_write_us(void);

// Decoded state for the radio task. regs_mhz() = 0 if register 0x01
// decodes to a frequency outside REGS_MIN_MHZ..REGS_MAX_MHZ.
uint32_t regs_mhz(void);
bool regs_powered_down(void);
void regs_ext(ext_state_t *out);
// True once after any write that changed the frequency, power or EXT state.
bool regs_take_changes(void);

// RX5808 register 0x01 encoding, as hosts compute it.
uint32_t regs_encode_mhz(uint32_t mhz);
