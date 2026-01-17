// nRF52 Early Boot Register Capture
//
// This file captures RESETREAS and GPREGRET registers before SystemInit()
// clears them. These registers tell us why the device reset and why it
// entered SYSTEMOFF (if applicable).
//
// The __attribute__((constructor(101))) ensures this runs before main()
// and before the Arduino/nRF SDK initialization.

#ifdef NRF52_POWER_MANAGEMENT

#include "nrf.h"

// Global storage for pre-SoftDevice register capture
// These are read by Nrf52PowerMgt::initState() during board initialization
uint32_t g_nrf52_reset_reason = 0;
uint8_t g_nrf52_shutdown_reason = 0;

// Constructor with highest priority (101) to run before anything else
// This runs before SystemInit(), initVariant(), and all other initialization
static void __attribute__((constructor(101))) nrf52_early_reset_capture() {
  g_nrf52_reset_reason = NRF_POWER->RESETREAS;
  g_nrf52_shutdown_reason = NRF_POWER->GPREGRET;
}

#endif // NRF52_POWER_MANAGEMENT
