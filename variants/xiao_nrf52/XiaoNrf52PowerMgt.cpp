#include "XiaoNrf52PowerMgt.h"
#include "variant.h"
#include <nrf_soc.h>

// Global flag - set to true by application firmware that implements power management
bool power_mgmt_implemented = false;

namespace XiaoNrf52PowerMgt {

  // Initialize power management
  void initialize() {
    // Phase 1a: Minimal initialization
    // Future phases will add:
    // - POF configuration (Phase 5)
    // - LPCOMP configuration (Phase 6)
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
    uint32_t usb_status;
    sd_power_usbregstatus_get(&usb_status);
    return (usb_status & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
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

  // Check if battery voltage is sufficient for safe boot
  // Phase 2 implementation - placeholder for now
  bool checkBootVoltage(uint16_t threshold_mv) {
    // Will be implemented in Phase 2
    return true;  // Allow boot for now
  }

  // Enter SYSTEMOFF mode (low power)
  // Phase 2 basic implementation, Phase 6 adds LPCOMP wake
  void enterSystemOff() {
    Serial.println("Entering SYSTEMOFF mode");
    delay(100);  // Allow serial to flush

    // Keep VBAT divider enabled for future LPCOMP wake (Phase 6)
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);

    // Enter SYSTEMOFF (never returns)
    sd_power_system_off();
    while(1);  // Should never reach here
  }

} // namespace XiaoNrf52PowerMgt
