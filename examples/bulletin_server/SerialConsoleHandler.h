#pragma once

#include <Arduino.h>

// Forward declarations
class MyMesh;

#ifndef MAX_SERIAL_COMMAND_LEN
  #define MAX_SERIAL_COMMAND_LEN 160
#endif

/**
 * Serial Console Handler - processes serial input for CLI commands
 *
 * Reads characters from Serial, accumulates into a buffer, and
 * dispatches complete lines to MyMesh::handleCommand().
 */
class SerialConsoleHandler {
public:
  SerialConsoleHandler(MyMesh* mesh);

  /**
   * Process available serial input
   * Call this from loop() to handle incoming serial commands
   */
  void loop();

private:
  MyMesh* _mesh;
  char _command[MAX_SERIAL_COMMAND_LEN + 1];
};
