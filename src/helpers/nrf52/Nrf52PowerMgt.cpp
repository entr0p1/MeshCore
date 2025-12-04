#include "Nrf52PowerMgt.h"
#include <nrf_soc.h>

// Global flag - set to true by application firmware that implements power management
bool power_mgmt_implemented = false;

namespace Nrf52PowerMgt {

  // Initialize power management
  void initialize() {

  }

  // Transition to a new power management state
  void transitionToState(PowerMgtState* state, uint8_t new_state) {
    if (state == nullptr) {
      return;
    }

    state->state_last = state->state_current;
    state->state_last_timestamp = state->state_current_timestamp;
    state->state_current = new_state;
    state->state_current_timestamp = millis();
  }

  // Get human-readable state string
  const char* getStateString(uint8_t state) {
    switch(state) {
      case PWRMGT_STATE_NORMAL:    return "Normal";
      case PWRMGT_STATE_CONSERVE:  return "Conserve";
      case PWRMGT_STATE_SLEEP:     return "Sleep";
      case PWRMGT_STATE_SHUTDOWN:  return "Shutdown";
      default:                     return "Unknown";
    }
  }

  // Check if external power (USB or 5V rail) is present
  bool isExternalPowered() {
    // Check if SoftDevice is enabled before using its API
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);

    if (sd_enabled) {
      // Use SoftDevice API when available
      uint32_t usb_status;
      sd_power_usbregstatus_get(&usb_status);
      return (usb_status & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    } else {
      // SoftDevice not available, read register directly
      return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    }
  }

  // Get human-readable reset reason string
  const char* getResetReasonString(uint32_t reset_reason) {
    if (reset_reason & POWER_RESETREAS_RESETPIN_Msk) return "Reset Pin";
    if (reset_reason & POWER_RESETREAS_DOG_Msk) return "Watchdog";
    if (reset_reason & POWER_RESETREAS_SREQ_Msk) return "Soft Reset";
    if (reset_reason & POWER_RESETREAS_LOCKUP_Msk) return "CPU Lockup";
    if (reset_reason & POWER_RESETREAS_OFF_Msk) return "Wake from SYSTEMOFF";
    if (reset_reason & POWER_RESETREAS_DIF_Msk) return "Debug Interface";
    return "Cold Boot";
  }

  // Enter SYSTEMOFF mode (low power)
  void enterSystemOff() {
    Serial.println("Entering SYSTEMOFF mode");
    delay(100);  // Allow serial to flush

    // Enter SYSTEMOFF (never returns)
    // Note: Variants with VBAT_ENABLE should configure it before calling this
    sd_power_system_off();
    while(1);  // Should never reach here
  }

} // namespace Nrf52PowerMgt
