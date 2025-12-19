#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/NRF52Board.h>
#include <helpers/nrf52/Nrf52PowerMgt.h>

#ifdef XIAO_NRF52

class XiaoNrf52Board : public NRF52BoardDCDC, public NRF52BoardOTA {
protected:
  uint32_t startup_reason;              // RESETREAS register value
  uint16_t boot_voltage_mv;             // Battery voltage at boot (millivolts)
  unsigned long last_voltage_check_ms;  // Timestamp of last voltage monitoring check
  uint8_t led_power_state;              // Track LED state to avoid redundant updates

  PowerMgtState power_state;    // Power management state

  // Helper to set RGB LEDs based on power state (for dev/testing)
  // Visual scheme: Normal=off, Conserve=Yellow(R+G), Sleep=Blue, Shutdown=Red
  void setPowerStateLED(uint8_t state);

  // Board-specific shutdown preparation (VBAT divider, peripherals)
  void prepareForBoardShutdown() {
    // Keep VBAT divider enabled for future LPCOMP wake
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);
    // Future: GPS, sensors, SPI cleanup
  }

public:
  XiaoNrf52Board() : NRF52BoardOTA("XIAO_NRF52_OTA") {}
  void begin();
  void loop();  // Periodic tasks (voltage monitoring, LED updates)

  // Returns true if board is in deep sleep cycle (RTC wake timer active)
  // Call at start of main loop; if true, skip normal processing
  bool isInDeepSleep() override;

  uint8_t getStartupReason() const override {
    return BD_STARTUP_NORMAL;  // Legacy interface, real reason in startup_reason
  }

  uint32_t getResetReason() const { return startup_reason; }

  uint16_t getBootVoltage() override {
    return boot_voltage_mv;
  }

#if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED on
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED off
  }
#endif

  uint16_t getBattMilliVolts() override;

  const char* getManufacturerName() const override {
    return "Seeed Xiao-nrf52";
  }

  void powerOff() override {
    // Set LED on and wait for button release before poweroff
    digitalWrite(PIN_LED, LOW);
#ifdef PIN_USER_BTN
    while(digitalRead(PIN_USER_BTN) == LOW);
#endif
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(PIN_LED, HIGH);

#ifdef PIN_USER_BTN
    // Configure button press to wake up when in powered off state
    nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_USER_BTN]), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
#endif

    // Use standard shutdown sequence
    prepareForBoardShutdown();
    Nrf52PowerMgt::prepareForShutdown();
    Nrf52PowerMgt::enterSystemOff();
  }

  bool supportsPowerManagement() override {
    return true;
  }

  bool isExternalPowered() override {
    return Nrf52PowerMgt::isExternalPowered();
  }

  const char* getResetReasonString() override {
    return Nrf52PowerMgt::getResetReasonString(startup_reason);
  }

  // Power management state access
  uint8_t getPowerState() const {
    return power_state.state_current;
  }

  const char* getPowerStateString() const {
    return PowerMgt::getStateString(power_state.state_current);
  }

  void getPwrMgtCurrentStateInfo(char* buffer, size_t buflen) const override {
    unsigned long elapsed_sec = (millis() - power_state.state_current_timestamp) / 1000;
    snprintf(buffer, buflen, "%s (for %lu sec)",
             PowerMgt::getStateString(power_state.state_current), elapsed_sec);
  }

  void getPwrMgtLastStateInfo(char* buffer, size_t buflen) const override {
    unsigned long elapsed_sec = (millis() - power_state.state_last_timestamp) / 1000;
    snprintf(buffer, buflen, "%s (%lu sec ago)",
             PowerMgt::getStateString(power_state.state_last), elapsed_sec);
  }

  bool setPwrMgtState(uint8_t state) override {
    if (state > PowerMgt::STATE_SHUTDOWN) {
      return false;
    }
    Nrf52PowerMgt::transitionToState(&power_state, state);

    // Handle shutdown state - gracefully power off
    if (state == PowerMgt::STATE_SHUTDOWN) {
      prepareForBoardShutdown();
      Nrf52PowerMgt::prepareForShutdown();
      Nrf52PowerMgt::enterSystemOff();
    }

    return true;
  }
};

#endif
