#include "SerialConsoleHandler.h"
#include "MyMesh.h"

SerialConsoleHandler::SerialConsoleHandler(MyMesh* mesh) : _mesh(mesh) {
  _command[0] = 0;
}

void SerialConsoleHandler::loop() {
  int len = strlen(_command);

  // Read available characters into command buffer
  while (Serial.available() && len < MAX_SERIAL_COMMAND_LEN) {
    char c = Serial.read();
    if (c != '\n') {
      _command[len++] = c;
      _command[len] = 0;
    }
    Serial.print(c);  // Echo character
  }

  // Handle buffer full condition
  if (len == MAX_SERIAL_COMMAND_LEN) {
    _command[MAX_SERIAL_COMMAND_LEN] = '\r';
    len++;
  }

  // Process complete line (ends with carriage return)
  if (len > 0 && _command[len - 1] == '\r') {
    _command[len - 1] = 0;  // Replace CR with null terminator

    char reply[MAX_SERIAL_COMMAND_LEN];
    _mesh->handleCommand(0, _command, reply);  // 0 = serial (no sender timestamp)

    if (reply[0]) {
      Serial.print("  -> ");
      Serial.println(reply);
    }

    _command[0] = 0;  // Reset command buffer
  }
}
