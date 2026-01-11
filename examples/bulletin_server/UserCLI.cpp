#include "UserCLI.h"
#include "MyMesh.h"

UserCLI::UserCLI(MyMesh* mesh) : _mesh(mesh) {}

bool UserCLI::handleCommand(ClientInfo* client, mesh::Packet* packet, const char* command, char* reply) {
  // Check if this is a user command (starts with '!')
  if (command[0] != '!') {
    return false;  // Not a user command
  }

  const char* cmd = &command[1];  // Skip '!' prefix
  uint32_t timestamp = _mesh->getRTCClock()->getCurrentTime();

  // Log request
  _mesh->logUserCommand("request", cmd, client->id.pub_key, timestamp);

  // Handle specific commands
  if (strcmp(cmd, "help") == 0) {
    strcpy(reply, "Commands:\n!help [cmd]\n!version\n!channel\n!channelkey\n!rxp\n!txp\n!app <app_name> <command>");
  } else if (strncmp(cmd, "help ", 5) == 0) {
    cmdHelp(&cmd[5], reply);
  } else if (strcmp(cmd, "version") == 0) {
    cmdVersion(reply);
  } else if (strcmp(cmd, "channel") == 0) {
    cmdChannel(reply);
  } else if (strcmp(cmd, "channelkey") == 0) {
    cmdChannelKey(reply);
  } else if (strcmp(cmd, "rxp") == 0) {
    cmdRxPath(packet, reply);
  } else if (strcmp(cmd, "txp") == 0) {
    cmdTxPath(client, reply);
  } else if (strcmp(cmd, "app") == 0) {
    strcpy(reply, "Usage: !app <app_name> <command>\nSends command to external application.");
  } else if (strncmp(cmd, "app ", 4) == 0) {
    if (cmdApp(client, &cmd[4], reply)) {
      return true;  // App command handles its own logging
    }
  } else {
    strcpy(reply, "Unknown command. Type !help for list.");
  }

  // Log response
  _mesh->logUserCommand("response", reply, client->id.pub_key, timestamp);

  return true;  // Was a user command
}

bool UserCLI::cmdHelp(const char* help_cmd, char* reply) {
  if (strcmp(help_cmd, "version") == 0) {
    strcpy(reply, "!version: Display firmware and MeshCore version info");
  } else if (strcmp(help_cmd, "channel") == 0) {
    strcpy(reply, "!channel: Display current broadcast channel mode (public/private)");
  } else if (strcmp(help_cmd, "channelkey") == 0) {
    strcpy(reply, "!channelkey: Display the channel encryption key (hex)");
  } else if (strcmp(help_cmd, "rxp") == 0) {
    strcpy(reply, "!rxp: Display the receive path (route from you to server)");
  } else if (strcmp(help_cmd, "txp") == 0) {
    strcpy(reply, "!txp: Display the transmit path (route from server to you)");
  } else if (strcmp(help_cmd, "app") == 0) {
    strcpy(reply, "!app <app_name> <command>: Send command to external application");
  } else {
    strcpy(reply, "Unknown command. Type !help for list.");
  }
  return true;
}

bool UserCLI::cmdVersion(char* reply) {
  sprintf(reply, "Firmware: %s (%s)\nMeshCore: %s\nRole: %s",
          FIRMWARE_VERSION, FIRMWARE_BUILD_DATE, MESHCORE_VERSION, FIRMWARE_ROLE);
  return true;
}

bool UserCLI::cmdChannel(char* reply) {
  if (_mesh->isChannelPrivate()) {
    strcpy(reply, "Mode: private\nUse !channelkey to print key.");
  } else {
    strcpy(reply, "Mode: public\nChannel is using server's public key. Use !channelkey to print key.");
  }
  return true;
}

bool UserCLI::cmdChannelKey(char* reply) {
  _mesh->formatChannelKey(reply, MAX_USER_REPLY_SIZE);
  return true;
}

bool UserCLI::cmdRxPath(mesh::Packet* packet, char* reply) {
  if (packet->isRouteFlood()) {
    sprintf(reply, "RX Path: FLOOD (path_len=%d)", packet->path_len);
    if (packet->path_len > 0) {
      char* pos = reply + strlen(reply);
      strcpy(pos, " [");
      pos += 2;
      for (int i = 0; i < packet->path_len && i < 6; i++) {
        sprintf(pos, "%02X", packet->path[i]);
        pos += 2;
        if (i < packet->path_len - 1) *pos++ = ' ';
      }
      *pos++ = ']';
      *pos = 0;
    }
  } else {
    // DIRECT routing - check if zero-hop or consumed path
    if (packet->path_len == 0) {
      strcpy(reply, "RX Path: DIRECT (zero-hop)");
    } else {
      strcpy(reply, "RX Path: DIRECT (consumed)");
    }
  }
  return true;
}

bool UserCLI::cmdTxPath(ClientInfo* client, char* reply) {
  if (client->out_path_len < 0) {
    strcpy(reply, "TX Path: FLOOD (path unknown)");
  } else if (client->out_path_len == 0) {
    strcpy(reply, "TX Path: DIRECT (zero-hop)");
  } else {
    sprintf(reply, "TX Path: DIRECT [");
    char* pos = reply + strlen(reply);
    for (int i = 0; i < client->out_path_len && i < 6; i++) {
      sprintf(pos, "%02X", client->out_path[i]);
      pos += 2;
      if (i < client->out_path_len - 1) *pos++ = ' ';
    }
    *pos++ = ']';
    *pos = 0;
  }
  return true;
}

bool UserCLI::cmdApp(ClientInfo* client, const char* args, char* reply) {
  // Skip leading spaces
  const char* app_data = args;
  while (*app_data == ' ') app_data++;

  if (*app_data == 0) {
    strcpy(reply, "Usage: !app <app_name> <command>");
    return false;  // Let caller log the response
  }

  // Parse app_name and command (split at first space)
  const char* app_name = app_data;
  const char* app_command = strchr(app_data, ' ');

  if (app_command == NULL) {
    strcpy(reply, "Usage: !app <app_name> <command>");
    return false;  // Let caller log the response
  }

  // Copy app_name into a temporary buffer
  char app_name_buf[64];
  int app_name_len = app_command - app_name;
  if (app_name_len >= (int)sizeof(app_name_buf)) app_name_len = sizeof(app_name_buf) - 1;
  memcpy(app_name_buf, app_name, app_name_len);
  app_name_buf[app_name_len] = 0;

  // Skip spaces after app_name
  app_command++;
  while (*app_command == ' ') app_command++;
  if (*app_command == 0) {
    strcpy(reply, "Usage: !app <app_name> <command>");
    return false;  // Let caller log the response
  }

  // Output JSON request for external app
  Serial.print("{\"component\":\"app\",\"action\":\"request\",\"data\":{\"app_name\":\"");
  Serial.print(app_name_buf);
  Serial.print("\",\"command\":\"");
  Serial.print(app_command);
  Serial.print("\"},\"meta\":{\"user_pubkey\":\"");
  mesh::Utils::printHex(Serial, client->id.pub_key, PUB_KEY_SIZE);
  Serial.print("\",\"source\":\"mesh\",\"timestamp\":");
  Serial.print(_mesh->getRTCClock()->getCurrentTime());
  Serial.println("}}");

  // Mark pending app request
  _mesh->markPendingAppRequest(client);

  // Send immediate "Processing..." response to user
  strcpy(reply, "Processing request...");
  return true;  // Skip normal response logging (app handles its own)
}
