#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
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

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Room ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  sensors.begin();

  // Initialize SD card storage (ESP32 with SD_CS_PIN only)
  SDStorage* sd_storage = nullptr;
#if defined(ESP32) && defined(SD_CS_PIN)
  static SDStorage sd;
  if (sd.begin()) {
    sd_storage = &sd;
  } else {
    Serial.println("SD card not available - storage features disabled");
  }
#endif

  the_mesh.begin(fs, sd_storage);

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
