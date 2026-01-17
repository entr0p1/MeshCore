#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/NRF52Board.h>
#include <helpers/nrf52/Nrf52PowerMgt.h>

#ifdef XIAO_NRF52

class XiaoNrf52Board : public NRF52BoardDCDC, public NRF52BoardOTA {
protected:
#ifdef NRF52_POWER_MANAGEMENT
  PowerMgtState power_state;
  static const PowerMgtConfig power_config;

  // Board-specific callbacks for power management
  static uint16_t readBatteryVoltageCallback();
  static void prepareShutdownCallback();
#endif

public:
  XiaoNrf52Board() : NRF52BoardOTA("XIAO_NRF52_OTA") {}
  void begin();
  void loop();

  // Returns true if board is in deep sleep cycle (skip normal processing)
  bool isInDeepSleep() override;

#if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, HIGH);
  }
#endif

  uint16_t getBattMilliVolts() override;

  const char* getManufacturerName() const override {
    return "Seeed Xiao-nrf52";
  }

  void powerOff() override;

  // Power management interface
#ifdef NRF52_POWER_MANAGEMENT
  bool supportsPowerManagement() override { return true; }

  bool isExternalPowered() override {
    return Nrf52PowerMgt::isExternalPowered();
  }

  uint16_t getBootVoltage() override {
    return power_state.boot_voltage_mv;
  }

  const char* getResetReasonString() override {
    return Nrf52PowerMgt::getResetReasonString(power_state.reset_reason);
  }

  uint8_t getShutdownReason() const override {
    return power_state.shutdown_reason;
  }

  const char* getShutdownReasonString() override {
    return Nrf52PowerMgt::getShutdownReasonString(power_state.shutdown_reason);
  }

  void getPwrMgtCurrentStateInfo(char* buffer, size_t buflen) const override {
    unsigned long elapsed_sec = (millis() - power_state.state_current_timestamp) / 1000;
    snprintf(buffer, buflen, "%s (Started %lu sec ago)",
             PowerMgt::getStateString(power_state.state_current), elapsed_sec);
  }

  void getPwrMgtLastStateInfo(char* buffer, size_t buflen) const override {
    unsigned long elapsed_sec = (millis() - power_state.state_last_timestamp) / 1000;
    snprintf(buffer, buflen, "%s (Started %lu sec ago)",
             PowerMgt::getStateString(power_state.state_last), elapsed_sec);
  }

  bool setPwrMgtState(uint8_t state) override {
    if (state > PowerMgt::STATE_SHUTDOWN) {
      return false;
    }
    Nrf52PowerMgt::transitionToState(&power_state, state);

    // Handle shutdown state - gracefully power off
    if (state == PowerMgt::STATE_SHUTDOWN) {
      prepareShutdownCallback();
      Nrf52PowerMgt::configureLpcompWake(
        power_config.lpcomp_ain_channel,
        power_config.lpcomp_ref_eighths);
      Nrf52PowerMgt::enterSystemOff(SHUTDOWN_REASON_USER);
    }

    return true;
  }
#endif // NRF52_POWER_MANAGEMENT
};

#endif // XIAO_NRF52
