#ifdef XIAO_NRF52

#include <Arduino.h>
#include <Wire.h>

#include "XiaoNrf52Board.h"
#include "target.h"

#ifdef NRF52_POWER_MANAGEMENT
#include <helpers/PowerMgt.h>

// Static configuration for power management
// Values come from variant.h defines
const PowerMgtConfig XiaoNrf52Board::power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_ref_eighths = PWRMGT_LPCOMP_REF_EIGHTHS,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK,
  .voltage_conserve = PWRMGT_VOLTAGE_CONSERVE,
  .voltage_sleep = PWRMGT_VOLTAGE_SLEEP,
  .voltage_shutdown = PWRMGT_VOLTAGE_SHUTDOWN,
  .readBatteryVoltage = &XiaoNrf52Board::readBatteryVoltageCallback,
  .prepareShutdown = &XiaoNrf52Board::prepareShutdownCallback,
};

// Static callback: read battery voltage via ADC
uint16_t XiaoNrf52Board::readBatteryVoltageCallback() {
  int adcvalue = analogRead(PIN_VBAT);
  return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
}

// Static callback: prepare board for SYSTEMOFF
void XiaoNrf52Board::prepareShutdownCallback() {
  // Keep VBAT divider enabled for LPCOMP monitoring during SYSTEMOFF
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
}
#endif // NRF52_POWER_MANAGEMENT

void XiaoNrf52Board::begin() {
  // Call base class begin() for DC/DC and common init
  NRF52BoardDCDC::begin();

  Serial.begin(115200);
  delay(1000);  // Wait for serial console to init

  // Configure battery voltage ADC pin and settings
  pinMode(PIN_VBAT, INPUT);
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  delay(50);  // Allow ADC to settle before first read

#ifdef NRF52_POWER_MANAGEMENT
  // Initialize power management state from early-captured registers
  Nrf52PowerMgt::initState(&power_state);

  // Boot voltage protection check (may not return if voltage too low)
  Nrf52PowerMgt::checkBootVoltage(&power_state, &power_config);

  // Enable power management for this board
  PowerMgt::setAvailable(true);
#endif

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

void XiaoNrf52Board::loop() {
#ifdef NRF52_POWER_MANAGEMENT
  // Periodic voltage monitoring with state transitions
  Nrf52PowerMgt::monitorVoltage(&power_state, &power_config);
#endif
}

uint16_t XiaoNrf52Board::getBattMilliVolts() {
  int adcvalue = analogRead(PIN_VBAT);
  return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
}

void XiaoNrf52Board::powerOff() {
  // Visual feedback: LED on while waiting for button release
  digitalWrite(PIN_LED, LOW);
#ifdef PIN_USER_BTN
  while(digitalRead(PIN_USER_BTN) == LOW);
#endif
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
  digitalWrite(PIN_LED, HIGH);

#ifdef PIN_USER_BTN
  // Configure button press to wake from SYSTEMOFF
  nrf_gpio_cfg_sense_input(g_ADigitalPinMap[PIN_USER_BTN], NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
#endif

#ifdef NRF52_POWER_MANAGEMENT
  // Use centralized shutdown with LPCOMP wake
  prepareShutdownCallback();
  Nrf52PowerMgt::configureLpcompWake(power_config.lpcomp_ain_channel, power_config.lpcomp_ref_eighths);
  Nrf52PowerMgt::enterSystemOff(SHUTDOWN_REASON_USER);
#else
  // Fallback: direct SYSTEMOFF without LPCOMP wake
  NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
  while(1) { __WFE(); }
#endif
}

bool XiaoNrf52Board::isInDeepSleep() {
#ifdef NRF52_POWER_MANAGEMENT
  return Nrf52PowerMgt::deepSleep(&power_state, rtc_clock, radio_driver);
#else
  return false;
#endif
}

#endif // XIAO_NRF52
