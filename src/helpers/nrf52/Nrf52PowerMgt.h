#pragma once

// nRF52 Power Management Module
//
// Centralized power management for nRF52-based boards. Provides:
// - Early boot register capture (RESETREAS, GPREGRET)
// - Boot voltage protection with LPCOMP wake
// - Runtime voltage monitoring with state transitions
// - Deep sleep with RTC synchronization
//
// Board variants enable this via -D NRF52_POWER_MANAGEMENT in platformio.ini
// and provide board-specific configuration via PowerMgtConfig.

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/PowerMgt.h>

#ifdef NRF52_POWER_MANAGEMENT

// ============================================================================
// Early Boot Capture (defined in Nrf52EarlyBoot.cpp)
// ============================================================================
// These globals are populated by a constructor that runs before SystemInit(),
// capturing register values before SoftDevice clears them.
extern uint32_t g_nrf52_reset_reason;
extern uint8_t g_nrf52_shutdown_reason;

// ============================================================================
// Shutdown Reason Codes (stored in GPREGRET before SYSTEMOFF)
// ============================================================================
#define SHUTDOWN_REASON_NONE          0x00
#define SHUTDOWN_REASON_LOW_VOLTAGE   0x4C  // 'L' - Runtime low voltage threshold
#define SHUTDOWN_REASON_USER          0x55  // 'U' - User requested powerOff()
#define SHUTDOWN_REASON_BOOT_PROTECT  0x42  // 'B' - Boot voltage protection

// ============================================================================
// Timing Constants
// ============================================================================
#define PWRMGT_STATE_SCAN_DEBOUNCE 3           // Consecutive readings for state change
#define PWRMGT_STATE_SCAN_DEBOUNCE_SHUTDOWN 2  // Consecutive readings for SYSTEMOFF
#define PWRMGT_STATE_SCAN_INTVL 1              // Minutes between voltage scans

// ============================================================================
// Board Configuration
// ============================================================================
// Boards provide this struct with their hardware-specific settings and callbacks.
// Thresholds set to 0 disable that feature.
struct PowerMgtConfig {
  // LPCOMP wake configuration (for voltage recovery from SYSTEMOFF)
  uint8_t lpcomp_ain_channel;       // AIN0-7 for voltage sensing pin
  uint8_t lpcomp_ref_eighths;       // VDD fraction: 0=1/8, 1=2/8, ..., 6=7/8

  // Voltage thresholds (millivolts) - set to 0 to disable
  uint16_t voltage_bootlock;        // Boot protection: won't boot below this
  uint16_t voltage_conserve;        // Runtime: enter Conserve mode below this
  uint16_t voltage_sleep;           // Runtime: enter Sleep mode below this
  uint16_t voltage_shutdown;        // Runtime: enter SYSTEMOFF below this

  // Board-specific callbacks
  uint16_t (*readBatteryVoltage)(); // Read battery voltage in mV (required)
  void (*prepareShutdown)();        // Board-specific shutdown prep (optional, e.g., enable VBAT divider for LPCOMP)
};

// ============================================================================
// Power Management State
// ============================================================================
// Boards hold an instance of this struct. Initialized by initState().
struct PowerMgtState {
  // Current power state
  uint8_t state_current;              // Current power state (PowerMgt::STATE_*)
  uint8_t state_last;                 // Previous power state
  unsigned long state_current_timestamp;  // millis() when current state entered
  unsigned long state_last_timestamp;     // millis() when previous state entered

  // Debounce for state transitions
  uint8_t state_scan_counter;         // Consecutive readings at target state
  uint8_t state_scan_target;          // Target state being debounced

  // Boot information (populated by initState)
  uint32_t reset_reason;              // RESETREAS register value
  uint8_t shutdown_reason;            // GPREGRET value (why we entered last SYSTEMOFF)
  uint16_t boot_voltage_mv;           // Battery voltage at boot (millivolts)

  // Runtime timing
  unsigned long last_voltage_check_ms;  // Timestamp of last voltage scan

  // Deep sleep state
  unsigned long last_sleep_millis;    // millis() when entering sleep
  uint32_t last_sleep_rtc;            // RTC time when entering sleep
  bool radio_is_sleeping;             // Radio powered off for sleep
  bool sleep_timer_inited;            // RTC2 sleep timer initialized
};

// ============================================================================
// Power Management API
// ============================================================================
namespace Nrf52PowerMgt {

  // --------------------------------------------------------------------------
  // Initialization (call in board::begin())
  // --------------------------------------------------------------------------

  // Initialize state from early-captured registers and clear registers for next boot.
  // Call this early in board::begin(), before checkBootVoltage().
  void initState(PowerMgtState* state);

  // Boot voltage protection check. Call after ADC is configured.
  // If voltage is below config->voltage_bootlock, configures LPCOMP wake and
  // enters SYSTEMOFF (does not return).
  // Returns true if boot can proceed, false if shutdown was triggered.
  // If voltage_bootlock is 0, always returns true (protection disabled).
  bool checkBootVoltage(PowerMgtState* state, const PowerMgtConfig* config);

  // --------------------------------------------------------------------------
  // Runtime Monitoring (call in board::loop())
  // --------------------------------------------------------------------------

  // Periodic voltage monitoring with debounced state transitions.
  // Reads voltage via config->readBatteryVoltage(), compares to thresholds,
  // and transitions state with hysteresis. On shutdown, calls config->prepareShutdown()
  // before entering SYSTEMOFF.
  void monitorVoltage(PowerMgtState* state, const PowerMgtConfig* config);

  // Deep sleep with RTC synchronization and radio management.
  // Returns true if in sleep mode (caller should skip normal loop processing).
  // Returns false if not in sleep mode (normal operation continues).
  bool deepSleep(PowerMgtState* state, mesh::RTCClock& rtc, mesh::Radio& radio);

  // --------------------------------------------------------------------------
  // State Management
  // --------------------------------------------------------------------------

  // Transition to a new power state (updates timestamps, notifies PowerMgt facade)
  void transitionToState(PowerMgtState* state, uint8_t new_state);

  // --------------------------------------------------------------------------
  // Hardware Utilities
  // --------------------------------------------------------------------------

  // Check if external power (USB VBUS) is present
  bool isExternalPowered();

  // Enter SYSTEMOFF mode with reason code in GPREGRET
  // Does not return on success. Reason is stored for next boot to read.
  void enterSystemOff(uint8_t reason);

  // Configure LPCOMP for voltage-based wake from SYSTEMOFF
  // Must be called before enterSystemOff() to enable voltage recovery wake.
  // ain_channel: AIN0-7, vdd_fraction_eighths: 0=1/8, 1=2/8, ..., 6=7/8
  void configureLpcompWake(uint8_t ain_channel, uint8_t vdd_fraction_eighths);

  // --------------------------------------------------------------------------
  // String Utilities (for CLI/debug output)
  // --------------------------------------------------------------------------

  // Human-readable string for RESETREAS register value
  const char* getResetReasonString(uint32_t reset_reason);

  // Human-readable string for GPREGRET shutdown reason code
  const char* getShutdownReasonString(uint8_t reason);

} // namespace Nrf52PowerMgt

#else // NRF52_POWER_MANAGEMENT not defined

// Provide empty struct definitions so board code can compile without #ifdefs everywhere
struct PowerMgtConfig {};
struct PowerMgtState {};

#endif // NRF52_POWER_MANAGEMENT
