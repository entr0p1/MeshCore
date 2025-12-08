#pragma once

// nRF52-specific power management implementation.
// Provides RTC-based sleep timer, external power detection, and shutdown handling.
// Board variants call these functions; applications use the PowerMgt facade.

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/PowerMgt.h>

// Scan/debounce constants
#define PWRMGT_STATE_SCAN_DEBOUNCE 3  // Consecutive readings to trigger state change
#define PWRMGT_STATE_SCAN_INTVL 5     // Minutes between voltage scans

// Power management state structure
struct PowerMgtState {
  uint8_t state_current;              // Current power state (0-3)
  uint8_t state_last;                 // Previous power state (0-3)
  unsigned long state_current_timestamp;  // When current state was entered
  unsigned long state_last_timestamp;     // When previous state was entered
  uint8_t state_scan_counter;         // Debounce counter
  uint8_t state_scan_target;          // Target state after scan

  // Deep sleep state
  unsigned long last_sleep_millis;    // millis() when entering sleep
  uint32_t last_sleep_rtc;            // RTC time when entering sleep
  bool radio_is_sleeping;             // Radio powered off for sleep
  bool sleep_timer_inited;            // RTC2 sleep timer initialized
};

// Power management functions for nRF52840
namespace Nrf52PowerMgt {
  // State management
  void transitionToState(PowerMgtState* state, uint8_t new_state);

  // Helper functions
  bool isExternalPowered();  // Check if USB or 5V rail powered
  const char* getResetReasonString(uint32_t reset_reason);  // Human-readable reset reason

  // Power state transitions
  void prepareForShutdown();  // Common nRF52 cleanup before shutdown (flash, serial, etc.)
  void enterSystemOff();  // Enter SYSTEMOFF mode (never returns)

  // Runtime voltage monitoring - call periodically from board loop
  void monitorVoltage(PowerMgtState* state, uint16_t current_voltage_mv,
                      void (*onShutdown)() = nullptr);

  // Deep sleep with RTC synchronization and radio management
  // Returns true if in SLEEP mode (load shedding active), false otherwise
  // Handles radio power, RTC sync, and CPU halt automatically
  bool deepSleep(PowerMgtState* state, mesh::RTCClock& rtc, mesh::Radio& radio);
}
