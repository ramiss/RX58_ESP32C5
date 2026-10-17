// SPDX-License-Identifier: GPL-3.0-only
// Minimal receive-only RF bring-up: makes MODEM_DIAG stream live I/Q.
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Wi-Fi as an RF front end only: 5 GHz, BW40, channel 173 (5865 MHz),
// promiscuous with a zero filter, MAC TX queues disabled, vendor AGC off,
// gain forced to `gain`, autonomous MODEM_DIAG writer running.
esp_err_t rf_min_start(uint8_t gain);

// The same front end without the autonomous MODEM_DIAG writer, for code that
// triggers its own RF-dump snapshots (src/hop_engine.c).
esp_err_t rf_min_start_front(uint8_t gain);

// Stop the modem writer and reboot into the ROM download mode. Needed to
// re-flash: esptool's USB reset does not reset the modem domain.
void rf_min_reboot_to_download(void);
