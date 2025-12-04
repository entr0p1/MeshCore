#ifdef XIAO_NRF52

#include <Arduino.h>
#include <Wire.h>
#include <bluefruit.h>

#include "XiaoNrf52Board.h"
#include "variant.h"

// Boot voltage protection threshold (millivolts)
// Conservative threshold provides headroom for voltage sag during TX
// LiPo cells should stay above 3.0V; 2.8V idle allows for 200-300mV sag under load
#define PWRMGT_BOOT_THRESHOLD_MV 2800

static BLEDfu bledfu;

static void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  MESH_DEBUG_PRINTLN("BLE client connected");
}

static void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;

  MESH_DEBUG_PRINTLN("BLE client disconnected");
}

void XiaoNrf52Board::begin() {
  // for future use, sub-classes SHOULD call this from their begin()

  // Use reset reason that was captured in early_boot.cpp constructor before SystemInit()
  startup_reason = g_reset_reason;

  // Debug: print the raw value and interpretation
  Serial.begin(115200);
  delay(1000); // Wait for serial console to init
  Serial.print("INIT: g_reset_reason = 0x");
  Serial.print(g_reset_reason, HEX);
  Serial.print(" - ");
  Serial.println(Nrf52PowerMgt::getResetReasonString(startup_reason));

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
    Serial.println("INIT: Power source = Battery");
    // Only shutdown if reading is valid (>1000mV) AND below threshold
    // This prevents spurious shutdowns on ADC glitches or uninitialized reads
    if (boot_voltage_mv > 1000 && boot_voltage_mv < PWRMGT_BOOT_THRESHOLD_MV) {
      Serial.print("INIT: CRITICAL: Battery voltage too low (");
      Serial.print(boot_voltage_mv);
      Serial.print(" mV < ");
      Serial.print(PWRMGT_BOOT_THRESHOLD_MV);
      Serial.println(" mV) - entering protective shutdown");
      delay(100);
      // Keep VBAT divider enabled for future LPCOMP wake (Phase 6)
      pinMode(VBAT_ENABLE, OUTPUT);
      digitalWrite(VBAT_ENABLE, LOW);
      // Enter SYSTEMOFF mode (never returns)
      Nrf52PowerMgt::enterSystemOff();
    } else {
      Serial.print("INIT: Battery voltage reading = ");
      Serial.print(boot_voltage_mv);
      Serial.println(" mV");
    }
  } else {
    // USB/5V powered - still read voltage for informational purposes
    Serial.println("INIT: Power source = External");
    Serial.print("INIT: Voltage reading = ");
    Serial.print(boot_voltage_mv);
    Serial.println(" mV");
  }

  // Enable DC/DC converter for improved power efficiency
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  } else {
    NRF_POWER->DCDCEN = 1;
  }

  // Initialize power management state
  power_state.state_current = PWRMGT_STATE_NORMAL;
  power_state.state_last = PWRMGT_STATE_NORMAL;
  power_state.state_current_timestamp = millis();
  power_state.state_last_timestamp = millis();
  power_state.state_scan_counter = 0;
  power_state.state_scan_target = 0;

  Nrf52PowerMgt::initialize();

#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT);
#endif

#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, HIGH);
#endif

  //  pinMode(SX126X_POWER_EN, OUTPUT);
  //  digitalWrite(SX126X_POWER_EN, HIGH);
  delay(10); // give sx1262 some time to power up
}

bool XiaoNrf52Board::startOTAUpdate(const char *id, char reply[]) {
  // Config the peripheral connection with maximum bandwidth
  // more SRAM required by SoftDevice
  // Note: All config***() function must be called before begin()
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(92, BLE_GAP_EVENT_LENGTH_MIN, 16, 16);

  Bluefruit.begin(1, 0);
  // Set max power. Accepted values are: -40, -30, -20, -16, -12, -8, -4, 0, 4
  Bluefruit.setTxPower(4);
  // Set the BLE device name
  Bluefruit.setName("XIAO_NRF52_OTA");

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // To be consistent OTA DFU should be added first if it exists
  bledfu.begin();

  // Set up and start advertising
  // Advertising packet
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addName();

  /* Start Advertising
    - Enable auto advertising if disconnected
    - Interval:  fast mode = 20 ms, slow mode = 152.5 ms
    - Timeout for fast mode is 30 seconds
    - Start(timeout) with timeout = 0 will advertise forever (until connected)

    For recommended advertising interval
    https://developer.apple.com/library/content/qa/qa1931/_index.html
  */
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
  Bluefruit.Advertising.start(0);             // 0 = Don't stop advertising after n seconds

  strcpy(reply, "OK - started");

  return true;
}

uint16_t XiaoNrf52Board::getBattMilliVolts() {
  int adcvalue = analogRead(PIN_VBAT);
  return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
}

#endif