#pragma once

// nRF52-specific power management implementation.
// Board variants call these functions; applications use the PowerMgt facade.

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/PowerMgt.h>

// Scan/debounce constants
// NOTE: These macros are intentionally simple so they can be tuned easily.
#define PWRMGT_STATE_SCAN_DEBOUNCE 3           // Consecutive readings to trigger state change (Conserve and sleep modes)
#define PWRMGT_STATE_SCAN_DEBOUNCE_SHUTDOWN 2  // Consecutive readings to trigger SYSTEMOFF (Shutdown mode only)
#define PWRMGT_STATE_SCAN_INTVL 1              // Minutes between voltage scans

// Shutdown reason codes (stored in GPREGRET before SYSTEMOFF)
#define SHUTDOWN_REASON_NONE          0x00
#define SHUTDOWN_REASON_LOW_VOLTAGE   0x4C  // 'L' - Software low voltage threshold
#define SHUTDOWN_REASON_USER          0x55  // 'U' - User requested powerOff()
#define SHUTDOWN_REASON_BOOT_PROTECT  0x42  // 'B' - Boot voltage protection

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

  // Enter SYSTEMOFF mode with cleanup
  void enterSystemOff(uint8_t reason);

  // Runtime voltage monitoring - call periodically from board loop
  void monitorVoltage(PowerMgtState* state, uint16_t current_voltage_mv,
                      void (*onShutdown)() = nullptr);

  // Deep sleep with RTC synchronization and radio management
  bool deepSleep(PowerMgtState* state, mesh::RTCClock& rtc, mesh::Radio& radio);

  // Shutdown reason (GPREGRET) - human-readable string
  const char* getShutdownReasonString(uint8_t reason);

  // LPCOMP wake-from-SYSTEMOFF configuration
  // Must be called before enterSystemOff() to enable voltage-based wake
  // ain_channel: AIN0-7 (0-7), vdd_fraction_eighths: 0=1/8, 1=2/8, ..., 6=7/8
  void configureLpcompWake(uint8_t ain_channel, uint8_t vdd_fraction_eighths);
}
