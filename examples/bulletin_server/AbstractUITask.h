#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

enum class UIEventType {
    none,
    roomMessage,
    ack
};

// Abstract interface between mesh networking and UI
// Bulletin server uses a pull model: UI queries MyMesh via getRecentPosts() rather than receiving pushed updates
class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  BaseSerialInterface* _serial;

  AbstractUITask(mesh::MainBoard* board, BaseSerialInterface* serial) : _board(board), _serial(serial) { }

public:
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void loop() = 0;
};
