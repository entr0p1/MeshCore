#ifndef NRF52_POWER_MGT_H
#define NRF52_POWER_MGT_H

#include <Arduino.h>

// Power management state definitions
#define PWRMGT_STATE_NORMAL 0
#define PWRMGT_STATE_CONSERVE 1
#define PWRMGT_STATE_SLEEP 2
#define PWRMGT_STATE_SHUTDOWN 3

// Scan/debounce constants
#define PWRMGT_STATE_SCAN_DEBOUNCE 3  // Consecutive readings to trigger state change
#define PWRMGT_STATE_SCAN_INTVL 2     // Minutes between voltage scans

// Global flag - set by application firmware if power management is implemented
extern bool power_mgmt_implemented;

// Power management state structure
struct PowerMgtState {
  uint8_t state_current;              // Current power state (0-3)
  uint8_t state_last;                 // Previous power state (0-3)
  unsigned long state_current_timestamp;  // When current state was entered
  unsigned long state_last_timestamp;     // When previous state was entered
  uint8_t state_scan_counter;         // Debounce counter
  uint8_t state_scan_target;          // Target state after scan
};

// Power management functions for nRF52840
namespace Nrf52PowerMgt {
  // Initialize power management (called from board begin())
  void initialize();

  // State management
  void transitionToState(PowerMgtState* state, uint8_t new_state);
  const char* getStateString(uint8_t state);

  // Helper functions
  bool isExternalPowered();  // Check if USB or 5V rail powered
  const char* getResetReasonString(uint32_t reset_reason);  // Human-readable reset reason
  void enterSystemOff();  // Enter SYSTEMOFF mode
}

#endif // NRF52_POWER_MGT_H
