#ifdef XIAO_NRF52

#include "nrf.h"

// External variables defined in variant.cpp
extern uint32_t g_reset_reason;
extern uint8_t g_shutdown_reason;

// Constructor with highest priority (101) to run before anything else
// This runs before SystemInit(), initVariant(), and all other initialization
static void __attribute__((constructor(101))) early_reset_capture() {
  // Capture RESETREAS and GPREGRET as early as possible (before SD clears them)
  g_reset_reason = NRF_POWER->RESETREAS;
  g_shutdown_reason = NRF_POWER->GPREGRET;
}

#endif
