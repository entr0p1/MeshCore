#ifdef XIAO_NRF52

#include <Arduino.h>
#include <Wire.h>

#include "XiaoNrf52Board.h"
#include "target.h"
#include <helpers/PowerMgt.h>

XiaoNrf52Board* XiaoNrf52Board::s_activeInstance = nullptr;

void XiaoNrf52Board::handleVoltageShutdown() {
  if (s_activeInstance) {
    s_activeInstance->prepareForBoardShutdown();
  }
}

void XiaoNrf52Board::begin() {
  s_activeInstance = this;

  // Call base class begin() for DC/DC and common init
  NRF52BoardDCDC::begin();

  // Use values captured in early_boot.cpp constructor before SystemInit()
  startup_reason = g_reset_reason;
  shutdown_reason = g_shutdown_reason;

  // Debug: print the raw value and interpretation
  Serial.begin(115200);
  delay(1000);  // Wait for serial console to init

  // Enhanced reset reason reporting - include shutdown reason for wake from SYSTEMOFF
  if ((startup_reason & POWER_RESETREAS_OFF_Msk) && shutdown_reason != SHUTDOWN_REASON_NONE) {
    MESH_DEBUG_PRINTLN("INIT: Reset = Wake from SYSTEMOFF (%s)",
      Nrf52PowerMgt::getShutdownReasonString(shutdown_reason));
  } else {
    MESH_DEBUG_PRINTLN("INIT: Reset = %s (0x%lX)",
      Nrf52PowerMgt::getResetReasonString(startup_reason), (unsigned long)startup_reason);
  }

  // Configure battery voltage ADC pin and settings (one-time initialization)
  pinMode(PIN_VBAT, INPUT);
  // VBAT_ENABLE already configured in variant.cpp initVariant() (set LOW for safety)
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  delay(50);  // Allow ADC to settle before first read

  // CRITICAL: Boot voltage protection check
  // Runs early (after Serial init) to protect battery from over-discharge
  // Skip check only if externally powered (USB/5V rail detected)
  // NOTE: SoftDevice not available yet, must read USB register directly
  bool usb_connected = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
  boot_voltage_mv = getBattMilliVolts();
  if (!usb_connected) {
    MESH_DEBUG_PRINTLN("INIT: Power source = Battery");
    // Only shutdown if reading is valid (>1000mV) AND below threshold
    // This prevents spurious shutdowns on ADC glitches or uninitialized reads
    if (boot_voltage_mv > 1000 && boot_voltage_mv < PWRMGT_VOLTAGE_BOOTLOCK) {
      MESH_DEBUG_PRINTLN("INIT: CRITICAL: Battery voltage too low (%u mV < %u mV) - entering protective shutdown",
        boot_voltage_mv, PWRMGT_VOLTAGE_BOOTLOCK);
      delay(100);
      prepareForBoardShutdown();
      // Enter SYSTEMOFF (never returns)
      Nrf52PowerMgt::enterSystemOff(SHUTDOWN_REASON_BOOT_PROTECT);
    } else {
      MESH_DEBUG_PRINTLN("INIT: Battery voltage reading = %u mV", boot_voltage_mv);
    }
  } else {
    // USB/5V powered - still read voltage for informational purposes
    MESH_DEBUG_PRINTLN("INIT: Power source = External");
    MESH_DEBUG_PRINTLN("INIT: Voltage reading = %u mV", boot_voltage_mv);
  }

  // Initialize power management state
  memset(&power_state, 0, sizeof(power_state));
  power_state.state_current = PowerMgt::STATE_NORMAL;
  power_state.state_last = PowerMgt::STATE_NORMAL;
  power_state.state_current_timestamp = millis();
  power_state.state_last_timestamp = millis();
  last_voltage_check_ms = millis();
  led_power_state = 0xFF;  // Initialize to invalid state to force first LED update

  // Enable power management for this board
  PowerMgt::setAvailable(true);

#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, HIGH);
#endif

  delay(10);  // Give sx1262 some time to power up
}

// Set RGB LEDs based on power state (for dev/testing without serial console)
// LEDs are active LOW on XIAO nRF52 (LOW=ON, HIGH=OFF)
// Visual scheme: Normal=off, Conserve=Yellow(R+G), Sleep=Blue, Shutdown=Red
// FIXME: remove LED-based state indicators before production (dev aid only)
void XiaoNrf52Board::setPowerStateLED(uint8_t state) {
  switch(state) {
    case PowerMgt::STATE_NORMAL:
      // Normal: All LEDs off to save power
      digitalWrite(LED_RED, HIGH);
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_BLUE, HIGH);
      break;

    case PowerMgt::STATE_CONSERVE:
      // Conserve: Yellow (Red + Green)
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_BLUE, HIGH);
      break;

    case PowerMgt::STATE_SLEEP:
      // Sleep: Blue
      digitalWrite(LED_RED, HIGH);
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_BLUE, LOW);
      break;

    case PowerMgt::STATE_SHUTDOWN:
      // Shutdown: Red
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_BLUE, HIGH);
      break;
  }
}

void XiaoNrf52Board::loop() {
  if (!PowerMgt::isAvailable()) {
    return;
  }
  if (!PowerMgt::isRuntimeEnabled()) {
    if (power_state.state_current != PowerMgt::STATE_NORMAL) {
      Nrf52PowerMgt::transitionToState(&power_state, PowerMgt::STATE_NORMAL);
    }
    return;
  }

  // Update LED indicator if power state changed
  // FIXME: remove LED-based state indicators before production (dev aid only)
  if (power_state.state_current != led_power_state) {
    setPowerStateLED(power_state.state_current);
    led_power_state = power_state.state_current;
  }

  // Periodic voltage monitoring (every PWRMGT_STATE_SCAN_INTVL minutes)
  unsigned long now = millis();
  unsigned long interval_ms = PWRMGT_STATE_SCAN_INTVL * 60UL * 1000UL;

  // Handle millis() rollover (occurs every ~49.7 days)
  if (now < last_voltage_check_ms) {
    last_voltage_check_ms = now;  // Reset on rollover
  }

  if (now - last_voltage_check_ms >= interval_ms) {
    last_voltage_check_ms = now;

    // Read current battery voltage
    uint16_t current_voltage = getBattMilliVolts();

    // Monitor voltage and handle state transitions
    // Use static member for board-specific shutdown callback
    Nrf52PowerMgt::monitorVoltage(&power_state, current_voltage, &XiaoNrf52Board::handleVoltageShutdown);
  }
}

uint16_t XiaoNrf52Board::getBattMilliVolts() {
  int adcvalue = analogRead(PIN_VBAT);
  return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
}

// Board-specific wrapper: passes variant globals to nRF52-generic deep sleep
bool XiaoNrf52Board::isInDeepSleep() {
  return Nrf52PowerMgt::deepSleep(&power_state, rtc_clock, radio_driver);
}

#endif
