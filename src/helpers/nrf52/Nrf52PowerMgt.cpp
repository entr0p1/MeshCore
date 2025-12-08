#include "Nrf52PowerMgt.h"
#include <nrf_soc.h>
#include <nrf_rtc.h>

namespace Nrf52PowerMgt {

  // Use RTC2 as a low-power wake timer in Sleep mode
  static void initSleepTimer() {
    static bool initialized = false;
    if (initialized) return;

    // 32.768 kHz / (PRESCALER+1); prescaler 32 -> ~1 kHz ticks
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

  extern "C" void RTC2_IRQHandler(void) {
    if (NRF_RTC2->EVENTS_COMPARE[0]) {
      NRF_RTC2->EVENTS_COMPARE[0] = 0;
      // No action needed; waking is enough
    }
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

    // Update neutral power management facade
    PowerMgt::setState(new_state);
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

  // Common nRF52 cleanup before shutdown
  // Call this before enterSystemOff() to gracefully close resources
  void prepareForShutdown() {
    MESH_DEBUG_PRINTLN("PWRMGT: Preparing for shutdown...");

    // Flush serial buffers before shutdown
    Serial.flush();
    delay(100);

    // Close filesystem if available
    // Note: Application must have already stopped writing to flash
#if defined(ARDUINO_ARCH_NRF52)
    #ifdef InternalFS
      // InternalFS.end();  // Uncomment when Adafruit_InternalFS supports end()
    #endif
#endif

    MESH_DEBUG_PRINTLN("PWRMGT: Shutdown preparation complete");
    Serial.flush();
    delay(50);
  }

  // Enter SYSTEMOFF mode (low power)
  void enterSystemOff() {
    MESH_DEBUG_PRINTLN("PWRMGT: Entering SYSTEMOFF mode");
    Serial.flush();
    delay(100);

    // Enter SYSTEMOFF
    // Note: Variants with VBAT_ENABLE should configure it before calling this
    sd_power_system_off();
    while(1);  // Should never reach here
  }

  // Runtime voltage monitoring with debouncing and state transitions
  // Call this periodically from board loop
  void monitorVoltage(PowerMgtState* state, uint16_t current_voltage_mv,
                      void (*onShutdown)()) {
    if (state == nullptr) {
      return;
    }

    // Skip if runtime PM disabled or backend not available
    if (!PowerMgt::isAvailable() || !PowerMgt::isRuntimeEnabled()) {
      if (state->state_current != PowerMgt::STATE_NORMAL) {
        transitionToState(state, PowerMgt::STATE_NORMAL);
      }
      return;
    }

    // Skip state transitions if externally powered
    if (isExternalPowered()) {
      // Reset to NORMAL when external power detected
      if (state->state_current != PowerMgt::STATE_NORMAL) {
        MESH_DEBUG_PRINTLN("PWRMGT: External power detected, returning to Normal mode");
        transitionToState(state, PowerMgt::STATE_NORMAL);
        state->state_scan_counter = 0;
      }
      return;
    }

    // Determine target state based on current voltage
    uint8_t target_state = PowerMgt::STATE_NORMAL;
    if (current_voltage_mv < PWRMGT_VOLTAGE_SHUTDOWN) {
      target_state = PowerMgt::STATE_SHUTDOWN;
    } else if (current_voltage_mv < PWRMGT_VOLTAGE_SLEEP) {
      target_state = PowerMgt::STATE_SLEEP;
    } else if (current_voltage_mv < PWRMGT_VOLTAGE_CONSERVE) {
      target_state = PowerMgt::STATE_CONSERVE;
    }

    // Debouncing logic: require consecutive readings before transition
    if (target_state != state->state_current) {
      if (state->state_scan_target == target_state) {
        // Same target as last scan, increment counter
        state->state_scan_counter++;

        if (state->state_scan_counter >= PWRMGT_STATE_SCAN_DEBOUNCE) {
          // Debounce threshold reached, transition to new state
          MESH_DEBUG_PRINTLN("PWRMGT: Voltage %u mV -> transitioning %s -> %s",
            current_voltage_mv,
            PowerMgt::getStateString(state->state_current),
            PowerMgt::getStateString(target_state));

          transitionToState(state, target_state);
          state->state_scan_counter = 0;

          // Handle shutdown state immediately
          if (target_state == PowerMgt::STATE_SHUTDOWN) {
            MESH_DEBUG_PRINTLN("PWRMGT: Critical battery level, entering shutdown");

            // Board-specific cleanup (GPS, sensors, peripherals, etc.)
            if (onShutdown != nullptr) {
              onShutdown();
            }

            // Common nRF52 cleanup (flash, serial, etc.)
            prepareForShutdown();

            // Enter SYSTEMOFF mode
            enterSystemOff();
          }
        }
      } else {
        // Different target than last scan
        // Only reset counter if new target is BETTER (lower state number)
        // If transitioning to a worse state (higher number), keep accumulating
        if (target_state < state->state_scan_target) {
          // Battery improved, reset counter
          state->state_scan_target = target_state;
          state->state_scan_counter = 0;
        } else {
          // Battery getting worse, update target but keep counter accumulating
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

  // Deep sleep with RTC synchronization and radio management
  // Returns true if in SLEEP mode (load shedding active), false otherwise
  // Handles radio power, RTC sync, and CPU halt automatically
  bool deepSleep(PowerMgtState* state, mesh::RTCClock& rtc, mesh::Radio& radio) {
    if (state == nullptr) {
      return false;
    }

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
        // Sync RTC: For hardware RTC this is harmless (sets to same time),
        // for software RTC this correctly advances the time
        rtc.setCurrentTime(state->last_sleep_rtc + elapsed_sec);
      }
    }

    // Record time before entering sleep
    state->last_sleep_millis = millis();
    state->last_sleep_rtc = rtc.getCurrentTime();

    // Enter deep sleep - CPU halts until next interrupt (timer, serial, SoftDevice)
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled) {
      sd_app_evt_wait();
    } else {
      // SoftDevice not running; use WFE/SEV sequence
      __SEV();
      __WFE();
      __WFE();
    }

    return true;  // Load shedding active, skip normal loop processing
  }

} // namespace Nrf52PowerMgt
