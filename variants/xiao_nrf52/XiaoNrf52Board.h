#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/nrf52/Nrf52PowerMgt.h>

#ifdef XIAO_NRF52

class XiaoNrf52Board : public mesh::MainBoard {
protected:
  uint32_t startup_reason;  // RESETREAS register value (captured in initVariant)
  uint16_t boot_voltage_mv; // Battery voltage at boot (millivolts)

public:
  PowerMgtState power_state;
  void begin();

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

  void reboot() override {
    NVIC_SystemReset();
  }

  void powerOff() override {
    // set led on and wait for button release before poweroff
    digitalWrite(PIN_LED, LOW);
#ifdef PIN_USER_BTN
    while(digitalRead(PIN_USER_BTN) == LOW);
#endif
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(PIN_LED, HIGH);

#ifdef PIN_USER_BTN
    // configure button press to wake up when in powered off state
    nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_USER_BTN]), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
#endif

    sd_power_system_off();
  }

  bool startOTAUpdate(const char* id, char reply[]) override;

  bool supportsPowerManagement() override {
    return true;
  }

  bool isExternalPowered() override {
    return Nrf52PowerMgt::isExternalPowered();
  }

  const char* getResetReasonString() override {
    return Nrf52PowerMgt::getResetReasonString(startup_reason);
  }
};

#endif