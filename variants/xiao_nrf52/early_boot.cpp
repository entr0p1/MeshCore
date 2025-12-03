#ifdef XIAO_NRF52

#include "nrf.h"

// External variable defined in variant.cpp
extern uint32_t g_reset_reason;

// Constructor with highest priority (101) to run before anything else
// This runs before SystemInit(), initVariant(), and all other initialization
static void __attribute__((constructor(101))) early_reset_capture() {
  // Capture RESETREAS as early as possible
  g_reset_reason = NRF_POWER->RESETREAS;
}

#endif
