#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#include "DataStore.h"
#include "SDStorage.h"
#include "SerialConsoleHandler.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  #include "AbstractUITask.h"

  // Stub serial interface (bulletin server doesn't use BLE/serial like companion_radio)
  class StubSerial : public BaseSerialInterface {
  public:
    bool isEnabled() const override { return false; }
    void enable() override {}
    void disable() override {}
    bool isConnected() const override { return false; }
    bool isWriteBusy() const override { return false; }
    size_t writeFrame(const uint8_t src[], size_t len) override { return 0; }
    size_t checkRecvFrame(uint8_t dest[]) override { return 0; }
  } stub_serial;

  UITask ui_task(&board, &stub_serial);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
static ArduinoMillis millis_clock;
MyMesh the_mesh(board, radio_driver, millis_clock, fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static SerialConsoleHandler* serial_console;

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_get_rng_seed());

  // Initialize filesystem
#if defined(NRF52_PLATFORM)
  InternalFS.begin();
  static DataStore data_store(InternalFS);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  static DataStore data_store(LittleFS);
#elif defined(ESP32)
  SPIFFS.begin(true);
  static DataStore data_store(SPIFFS);
#else
  #error "need to define filesystem"
#endif

  // Initialize SD card storage (optional, based on PIN_SDCARD_CS)
  SDStorage* sd_storage = nullptr;
#ifdef PIN_SDCARD_CS
  static SDStorage sd;
  sd.begin();  // Try to initialize - status will be set regardless of success
  sd_storage = &sd;  // Always pass pointer so status can be reported
#endif

  data_store.begin(sd_storage);

  // Load or create identity
  if (!data_store.loadMainIdentity(the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    data_store.saveMainIdentity(the_mesh.self_id);
  }

  Serial.print("Room ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  sensors.begin();

  the_mesh.begin(&data_store);

  // Initialize serial console handler
  serial_console = new SerialConsoleHandler(&the_mesh);

#ifdef DISPLAY_CLASS
  ui_task.begin(&display, &sensors, the_mesh.getNodePrefs());
#endif

  // send out initial Advertisement to the mesh
  the_mesh.sendSelfAdvertisement(16000);
}

void loop() {
#ifdef DISPLAY_CLASS
  // Notify UI of loaded posts on first loop iteration
  // newMsg() won't interrupt splash screen, so this is safe to call early
  static bool posts_notified = false;
  if (!posts_notified) {
    the_mesh.notifyUIOfLoadedPosts();
    posts_notified = true;
  }
#endif

  serial_console->loop();
  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
}
