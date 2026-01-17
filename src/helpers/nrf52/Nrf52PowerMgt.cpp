// nRF52 Power Management Implementation
//
// Centralized power management for nRF52-based boards.
// See Nrf52PowerMgt.h for API documentation.

#ifdef NRF52_POWER_MANAGEMENT

#include "Nrf52PowerMgt.h"
#include <nrf_soc.h>
#include <nrf_rtc.h>

namespace Nrf52PowerMgt {

  // ==========================================================================
  // RTC2 Sleep Timer (internal)
  // ==========================================================================

  static void initSleepTimer() {
    static bool initialized = false;
    if (initialized) return;

    // RTC2 uses LFCLK (32.768 kHz from LFXO or LFRC)
    // Accuracy varies by clock source but is sufficient for minute-scale wake intervals
    NRF_RTC2->PRESCALER = 32;
    NRF_RTC2->EVTENSET = RTC_EVTENSET_COMPARE0_Msk;
    NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;
    NRF_RTC2->TASKS_START = 1;
    NVIC_ClearPendingIRQ(RTC2_IRQn);
    NVIC_EnableIRQ(RTC2_IRQn);
    initialized = true;
  }

  static inline uint32_t msToRtcTicks(uint32_t ms) {
    // 32.768kHz / (PRESCALER+1) = 32768/33 = 993.2Hz, ~1.007ms per tick
    return (ms * 993UL) / 1000UL;
  }

  static void scheduleSleepWake(uint32_t interval_ms) {
    uint32_t now = NRF_RTC2->COUNTER;
    uint32_t ticks = msToRtcTicks(interval_ms);
    // RTC counter is 24-bit; wrap naturally
    NRF_RTC2->CC[0] = (now + ticks) & RTC_COUNTER_COUNTER_Msk;
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
  }

  // ==========================================================================
  // Initialization
  // ==========================================================================

  void initState(PowerMgtState* state) {
    if (state == nullptr) return;

    // Zero-initialize all fields
    memset(state, 0, sizeof(PowerMgtState));

    // Copy early-captured register values
    state->reset_reason = g_nrf52_reset_reason;
    state->shutdown_reason = g_nrf52_shutdown_reason;

    // Initialize state to normal
    state->state_current = PowerMgt::STATE_NORMAL;
    state->state_last = PowerMgt::STATE_NORMAL;
    state->state_current_timestamp = millis();
    state->state_last_timestamp = millis();
    state->last_voltage_check_ms = millis();

    // Clear registers for next boot (must be done before SoftDevice takes over)
    // Note: At this point SoftDevice may or may not be enabled
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled) {
      sd_power_reset_reason_clr(0xFFFFFFFF);
      sd_power_gpregret_clr(0, 0xFF);
    } else {
      NRF_POWER->RESETREAS = 0xFFFFFFFF;  // Write 1s to clear
      NRF_POWER->GPREGRET = 0;
    }

    // Log reset/shutdown info
    if (state->shutdown_reason != SHUTDOWN_REASON_NONE) {
      MESH_DEBUG_PRINTLN("PWRMGT: Reset = %s (0x%lX); Shutdown = %s (0x%02X)",
        getResetReasonString(state->reset_reason),
        (unsigned long)state->reset_reason,
        getShutdownReasonString(state->shutdown_reason),
        state->shutdown_reason);
    } else {
      MESH_DEBUG_PRINTLN("PWRMGT: Reset = %s (0x%lX)",
        getResetReasonString(state->reset_reason),
        (unsigned long)state->reset_reason);
    }
  }

  bool checkBootVoltage(PowerMgtState* state, const PowerMgtConfig* config) {
    if (state == nullptr || config == nullptr) return true;
    if (config->voltage_bootlock == 0) return true;  // Protection disabled
    if (config->readBatteryVoltage == nullptr) return true;  // No voltage reader

    // Skip check if externally powered
    if (isExternalPowered()) {
      MESH_DEBUG_PRINTLN("PWRMGT: Boot check skipped (external power)");
      state->boot_voltage_mv = config->readBatteryVoltage();
      return true;
    }

    // Read boot voltage
    state->boot_voltage_mv = config->readBatteryVoltage();
    MESH_DEBUG_PRINTLN("PWRMGT: Boot voltage = %u mV (threshold = %u mV)",
      state->boot_voltage_mv, config->voltage_bootlock);

    // Only trigger shutdown if reading is valid (>1000mV) AND below threshold
    // This prevents spurious shutdowns on ADC glitches or uninitialized reads
    if (state->boot_voltage_mv > 1000 && state->boot_voltage_mv < config->voltage_bootlock) {
      MESH_DEBUG_PRINTLN("PWRMGT: Boot voltage too low - entering protective shutdown");

      // Board-specific shutdown preparation
      if (config->prepareShutdown != nullptr) {
        config->prepareShutdown();
      }

      // Configure LPCOMP for voltage recovery wake
      configureLpcompWake(config->lpcomp_ain_channel, config->lpcomp_ref_eighths);

      // Enter SYSTEMOFF (does not return)
      enterSystemOff(SHUTDOWN_REASON_BOOT_PROTECT);
      return false;  // Never reached
    }

    return true;
  }

  // ==========================================================================
  // State Management
  // ==========================================================================

  void transitionToState(PowerMgtState* state, uint8_t new_state) {
    if (state == nullptr) return;

    state->state_last = state->state_current;
    state->state_last_timestamp = state->state_current_timestamp;
    state->state_current = new_state;
    state->state_current_timestamp = millis();

    // Update neutral power management facade
    PowerMgt::setState(new_state);
  }

  // ==========================================================================
  // Hardware Utilities
  // ==========================================================================

  bool isExternalPowered() {
    // Check if SoftDevice is enabled before using its API
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);

    if (sd_enabled) {
      uint32_t usb_status;
      sd_power_usbregstatus_get(&usb_status);
      return (usb_status & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    } else {
      return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
    }
  }

  // Internal helper: set shutdown reason in GPREGRET
  static void setShutdownReason(uint8_t reason) {
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled) {
      sd_power_gpregret_clr(0, 0xFF);
      sd_power_gpregret_set(0, reason);
    } else {
      NRF_POWER->GPREGRET = reason;
    }
  }

  void enterSystemOff(uint8_t reason) {
    MESH_DEBUG_PRINTLN("PWRMGT: Entering SYSTEMOFF (%s)",
      getShutdownReasonString(reason));

    // Record shutdown reason in GPREGRET
    setShutdownReason(reason);

    // Flush serial buffers
    Serial.flush();
    delay(100);

    // Enter SYSTEMOFF
    // IMPORTANT: sd_power_system_off() only works when the SoftDevice is enabled.
    // If called while SoftDevice is disabled, it returns NRF_ERROR_SOFTDEVICE_NOT_ENABLED
    // and execution continues, which previously caused an infinite loop.
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);

    if (sd_enabled) {
      uint32_t err = sd_power_system_off();
      if (err == NRF_ERROR_SOFTDEVICE_NOT_ENABLED) {
        sd_enabled = 0;
      }
    }

    if (!sd_enabled) {
      // SoftDevice not available; write directly to POWER->SYSTEMOFF
      NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
    }

    // If we get here, something went wrong. Halt in lowest-power state.
    while (1) {
      __WFE();
    }
  }

  void configureLpcompWake(uint8_t ain_channel, uint8_t vdd_fraction_eighths) {
    // LPCOMP is not managed by SoftDevice - direct register access required

    // Halt and disable before reconfiguration
    NRF_LPCOMP->TASKS_STOP = 1;
    NRF_LPCOMP->ENABLE = LPCOMP_ENABLE_ENABLE_Disabled;

    // Select analog input (AIN0-7 maps to PSEL 0-7)
    NRF_LPCOMP->PSEL = ((uint32_t)ain_channel << LPCOMP_PSEL_PSEL_Pos) & LPCOMP_PSEL_PSEL_Msk;

    // Reference: VDD fraction (0=1/8, 1=2/8, ..., 6=7/8)
    NRF_LPCOMP->REFSEL = ((uint32_t)vdd_fraction_eighths << LPCOMP_REFSEL_REFSEL_Pos) & LPCOMP_REFSEL_REFSEL_Msk;

    // Detect UP events (voltage rises above threshold for battery recovery)
    NRF_LPCOMP->ANADETECT = LPCOMP_ANADETECT_ANADETECT_Up;

    // Enable 50mV hysteresis for noise immunity
    NRF_LPCOMP->HYST = LPCOMP_HYST_HYST_Hyst50mV;

    // Clear stale events/interrupts before enabling wake
    NRF_LPCOMP->EVENTS_READY = 0;
    NRF_LPCOMP->EVENTS_DOWN = 0;
    NRF_LPCOMP->EVENTS_UP = 0;
    NRF_LPCOMP->EVENTS_CROSS = 0;

    NRF_LPCOMP->INTENCLR = 0xFFFFFFFF;
    NRF_LPCOMP->INTENSET = LPCOMP_INTENSET_UP_Msk;

    // Enable LPCOMP
    NRF_LPCOMP->ENABLE = LPCOMP_ENABLE_ENABLE_Enabled;
    NRF_LPCOMP->TASKS_START = 1;

    // Wait for comparator to settle before entering SYSTEMOFF
    for (uint8_t i = 0; i < 20 && !NRF_LPCOMP->EVENTS_READY; i++) {
      delayMicroseconds(50);
    }

    MESH_DEBUG_PRINTLN("PWRMGT: LPCOMP wake configured (AIN%d, ref=%d/8 VDD)",
      ain_channel, vdd_fraction_eighths + 1);
  }

  // ==========================================================================
  // Runtime Monitoring
  // ==========================================================================

  void monitorVoltage(PowerMgtState* state, const PowerMgtConfig* config) {
    if (state == nullptr || config == nullptr) return;
    if (config->readBatteryVoltage == nullptr) return;

    // Skip if runtime PM disabled or backend not available
    if (!PowerMgt::isAvailable() || !PowerMgt::isRuntimeEnabled()) {
      if (state->state_current != PowerMgt::STATE_NORMAL) {
        transitionToState(state, PowerMgt::STATE_NORMAL);
      }
      return;
    }

    // Check timing - only scan at configured interval
    unsigned long now = millis();
    unsigned long interval_ms = PWRMGT_STATE_SCAN_INTVL * 60UL * 1000UL;

    // Handle millis() rollover (occurs every ~49.7 days)
    if (now < state->last_voltage_check_ms) {
      state->last_voltage_check_ms = now;
    }

    if (now - state->last_voltage_check_ms < interval_ms) {
      return;  // Not time for a scan yet
    }
    state->last_voltage_check_ms = now;

    // Skip state transitions if externally powered
    if (isExternalPowered()) {
      if (state->state_current != PowerMgt::STATE_NORMAL) {
        MESH_DEBUG_PRINTLN("PWRMGT: External power detected, returning to Normal");
        transitionToState(state, PowerMgt::STATE_NORMAL);
        state->state_scan_counter = 0;
      }
      return;
    }

    // Read current battery voltage
    uint16_t current_voltage = config->readBatteryVoltage();

    // Determine target state based on voltage thresholds
    uint8_t target_state = PowerMgt::STATE_NORMAL;
    if (config->voltage_shutdown > 0 && current_voltage < config->voltage_shutdown) {
      target_state = PowerMgt::STATE_SHUTDOWN;
    } else if (config->voltage_sleep > 0 && current_voltage < config->voltage_sleep) {
      target_state = PowerMgt::STATE_SLEEP;
    } else if (config->voltage_conserve > 0 && current_voltage < config->voltage_conserve) {
      target_state = PowerMgt::STATE_CONSERVE;
    }

    // Debouncing logic: require consecutive readings before transition
    const uint8_t debounce_required =
      (target_state == PowerMgt::STATE_SHUTDOWN)
        ? PWRMGT_STATE_SCAN_DEBOUNCE_SHUTDOWN
        : PWRMGT_STATE_SCAN_DEBOUNCE;

    if (target_state != state->state_current) {
      if (state->state_scan_target == target_state) {
        // Same target as last scan, increment counter
        state->state_scan_counter++;

        if (state->state_scan_counter >= debounce_required) {
          // Debounce threshold reached, transition to new state
          MESH_DEBUG_PRINTLN("PWRMGT: Voltage %u mV -> transitioning %s -> %s",
            current_voltage,
            PowerMgt::getStateString(state->state_current),
            PowerMgt::getStateString(target_state));

          transitionToState(state, target_state);
          state->state_scan_counter = 0;

          // Handle shutdown state immediately
          if (target_state == PowerMgt::STATE_SHUTDOWN) {
            MESH_DEBUG_PRINTLN("PWRMGT: Critical battery level, entering shutdown");

            // Board-specific cleanup
            if (config->prepareShutdown != nullptr) {
              config->prepareShutdown();
            }

            // Configure LPCOMP wake for recovery
            configureLpcompWake(config->lpcomp_ain_channel, config->lpcomp_ref_eighths);

            enterSystemOff(SHUTDOWN_REASON_LOW_VOLTAGE);
          }
        }
      } else {
        // Different target than last scan
        // Only reset counter if new target is BETTER (lower state number)
        if (target_state < state->state_scan_target) {
          state->state_scan_target = target_state;
          state->state_scan_counter = 0;
        } else {
          // Battery getting worse, update target but keep accumulating
          state->state_scan_target = target_state;
          state->state_scan_counter++;
        }
      }
    } else {
      // Already in target state, reset debounce
      state->state_scan_counter = 0;
      state->state_scan_target = target_state;
    }
  }

  // ==========================================================================
  // Deep Sleep
  // ==========================================================================

  bool deepSleep(PowerMgtState* state, mesh::RTCClock& rtc, mesh::Radio& radio) {
    if (state == nullptr) return false;

    if (!PowerMgt::isAvailable() || !PowerMgt::isRuntimeEnabled()) {
      return false;
    }

    // Check if we're in SLEEP mode (state >= SLEEP)
    if (PowerMgt::getState() < PowerMgt::STATE_SLEEP) {
      // Not in SLEEP mode - restore radio if it was sleeping
      if (state->radio_is_sleeping) {
        radio.begin();
        state->radio_is_sleeping = false;
      }
      return false;  // Normal loop processing should continue
    }

    // We're in SLEEP mode (state >= 2)

    // First call in SLEEP mode - power off radio
    if (!state->radio_is_sleeping) {
      // Wait for any in-flight transmission to complete (max 100ms)
      unsigned long wait_start = millis();
      while (!radio.isSendComplete() && (millis() - wait_start < 100)) {
        delay(5);
      }
      radio.powerOff();
      state->radio_is_sleeping = true;
    }

    // Initialize and schedule wake timer
    if (!state->sleep_timer_inited) {
      initSleepTimer();
      state->sleep_timer_inited = true;
    }
    scheduleSleepWake(PWRMGT_STATE_SCAN_INTVL * 60UL * 1000UL);

    // On wake from previous sleep, sync RTC with elapsed time
    if (state->last_sleep_millis > 0) {
      unsigned long elapsed_ms = millis() - state->last_sleep_millis;
      uint32_t elapsed_sec = elapsed_ms / 1000;
      if (elapsed_sec > 0) {
        rtc.setCurrentTime(state->last_sleep_rtc + elapsed_sec);
      }
    }

    // Record time before entering sleep
    state->last_sleep_millis = millis();
    state->last_sleep_rtc = rtc.getCurrentTime();

    // Enter deep sleep - CPU halts until next interrupt
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled) {
      sd_app_evt_wait();
    } else {
      __SEV();
      __WFE();
      __WFE();
    }

    return true;  // Load shedding active, skip normal loop processing
  }

  // ==========================================================================
  // String Utilities
  // ==========================================================================

  const char* getResetReasonString(uint32_t reset_reason) {
    if (reset_reason & POWER_RESETREAS_RESETPIN_Msk) return "Reset Pin";
    if (reset_reason & POWER_RESETREAS_DOG_Msk) return "Watchdog";
    if (reset_reason & POWER_RESETREAS_SREQ_Msk) return "Soft Reset";
    if (reset_reason & POWER_RESETREAS_LOCKUP_Msk) return "CPU Lockup";
    #ifdef POWER_RESETREAS_LPCOMP_Msk
      if (reset_reason & POWER_RESETREAS_LPCOMP_Msk) return "Wake from LPCOMP";
    #endif
    #ifdef POWER_RESETREAS_VBUS_Msk
      if (reset_reason & POWER_RESETREAS_VBUS_Msk) return "Wake from VBUS";
    #endif
    #ifdef POWER_RESETREAS_OFF_Msk
      if (reset_reason & POWER_RESETREAS_OFF_Msk) return "Wake from GPIO";
    #endif
    #ifdef POWER_RESETREAS_DIF_Msk
      if (reset_reason & POWER_RESETREAS_DIF_Msk) return "Debug Interface";
    #endif
    return "Cold Boot";
  }

  const char* getShutdownReasonString(uint8_t reason) {
    switch (reason) {
      case SHUTDOWN_REASON_LOW_VOLTAGE:  return "Low Voltage";
      case SHUTDOWN_REASON_USER:         return "User Request";
      case SHUTDOWN_REASON_BOOT_PROTECT: return "Boot Protection";
      default:                           return "Unknown";
    }
  }

} // namespace Nrf52PowerMgt

// RTC2 IRQ Handler (must be in global scope)
extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    // No action needed; waking is enough
  }
}

#endif // NRF52_POWER_MANAGEMENT
