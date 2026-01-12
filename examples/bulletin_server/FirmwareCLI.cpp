#include "FirmwareCLI.h"
#include "MyMesh.h"
#include "DataStore.h"
#include "SDStorage.h"
#include <RTClib.h>

FirmwareCLI::FirmwareCLI(MyMesh* mesh) : _mesh(mesh) {}

bool FirmwareCLI::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  bool is_serial = (sender_timestamp == 0);

  // ACL commands
  if (memcmp(command, "setperm ", 8) == 0) {
    return cmdSetPerm(&command[8], reply);
  }
  if (strcmp(command, "get acl") == 0) {
    return cmdGetAcl(reply);
  }

  // Bulletin commands
  if (memcmp(command, "bulletin.", 9) == 0) {
    return cmdBulletin(&command[9], reply, is_serial);
  }

  // Network time sync commands
  if (memcmp(command, "set nettime.enable ", 19) == 0) {
    return cmdSetNettimeEnable(&command[19], reply);
  }
  if (strcmp(command, "get nettime.enable") == 0) {
    return cmdGetNettimeEnable(reply);
  }
  if (memcmp(command, "set nettime.maxwait ", 20) == 0) {
    return cmdSetNettimeMaxwait(&command[20], reply);
  }
  if (strcmp(command, "get nettime.maxwait") == 0) {
    return cmdGetNettimeMaxwait(reply);
  }
  if (strcmp(command, "get nettime.status") == 0) {
    return cmdGetNettimeStatus(reply);
  }

  // Channel commands
  if (strcmp(command, "get channel.mode") == 0) {
    return cmdGetChannelMode(reply);
  }
  if (memcmp(command, "set channel.mode ", 17) == 0) {
    return cmdSetChannelMode(&command[17], reply);
  }

  // Login history
  if (strcmp(command, "login.history") == 0) {
    return cmdLoginHistory(reply);
  }

  // App reply
  if (memcmp(command, "appreply ", 9) == 0) {
    return cmdAppReply(&command[9], reply);
  }

  // SD card commands
  if (strcmp(command, "erase.sd") == 0) {
    return cmdEraseSDCard(reply);
  }
  if (strcmp(command, "get sd.status") == 0) {
    return cmdGetSDStatus(reply);
  }

  return false; // Command not handled
}

bool FirmwareCLI::cmdSetPerm(const char* args, char* reply) {
  char* hex = (char*)args;
  char* sp = strchr(hex, ' ');
  if (sp == NULL) {
    strcpy(reply, "Err - bad params");
    return true;
  }

  *sp++ = 0;
  uint8_t pubkey[PUB_KEY_SIZE];
  int hex_len = min((int)(sp - hex), PUB_KEY_SIZE * 2);

  if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
    uint8_t perms = atoi(sp);
    if (_mesh->getACL()->applyPermissions(_mesh->self_id, pubkey, hex_len / 2, perms)) {
      _mesh->scheduleLazyWrite();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - invalid params");
    }
  } else {
    strcpy(reply, "Err - bad pubkey");
  }
  return true;
}

bool FirmwareCLI::cmdGetAcl(char* reply) {
  Serial.println("ACL:");
  ClientACL* acl = _mesh->getACL();
  for (int i = 0; i < acl->getNumClients(); i++) {
    auto c = acl->getClientByIdx(i);
    if (c->permissions == 0) continue;
    Serial.printf("%02X ", c->permissions);
    mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
    Serial.printf("\n");
  }
  reply[0] = 0;
  return true;
}

bool FirmwareCLI::cmdBulletin(const char* cmd, char* reply, bool is_serial) {
  if (_mesh->isDesynced()) {
    strcpy(reply, "ERROR: Clock not synced");
    return true;
  }

  PostSeverity severity;
  const char* message_start;

  if (strncmp(cmd, "info ", 5) == 0) {
    severity = SEVERITY_INFO;
    message_start = cmd + 5;
  } else if (strncmp(cmd, "warning ", 8) == 0) {
    severity = SEVERITY_WARNING;
    message_start = cmd + 8;
  } else if (strncmp(cmd, "critical ", 9) == 0) {
    severity = SEVERITY_CRITICAL;
    message_start = cmd + 9;
  } else {
    strcpy(reply, "ERROR: Invalid severity. Use bulletin.info|bulletin.warning|bulletin.critical");
    return true;
  }

  int text_len = strlen(message_start);
  if (text_len == 0) {
    strcpy(reply, "ERROR: Empty bulletin");
    return true;
  }
  if (text_len > MAX_POST_TEXT_LEN) {
    sprintf(reply, "ERROR: Max %d chars", MAX_POST_TEXT_LEN);
    return true;
  }

  if (!_mesh->checkBulletinRateLimit(reply)) {
    return true;
  }

  _mesh->addBulletin(message_start, severity);
  _mesh->updateBulletinRateLimit();

  if (is_serial) {
    reply[0] = '\0';
  } else {
    const char* sev_name = (severity == SEVERITY_INFO) ? "INFO" :
                           (severity == SEVERITY_WARNING) ? "WARNING" : "CRITICAL";
    sprintf(reply, "OK - %s bulletin posted", sev_name);
  }
  return true;
}

bool FirmwareCLI::cmdSetNettimeEnable(const char* val, char* reply) {
  if (strcmp(val, "on") == 0) {
    _mesh->setNetsyncEnabled(true);
    strcpy(reply, "OK - Network time sync enabled");
  } else if (strcmp(val, "off") == 0) {
    _mesh->setNetsyncEnabled(false);
    strcpy(reply, "OK - Network time sync disabled");
  } else {
    strcpy(reply, "Error: Use 'on' or 'off'");
  }
  return true;
}

bool FirmwareCLI::cmdGetNettimeEnable(char* reply) {
  strcpy(reply, _mesh->isNetsyncEnabled() ? "on" : "off");
  return true;
}

bool FirmwareCLI::cmdSetNettimeMaxwait(const char* val, char* reply) {
  int mins = atoi(val);
  if (mins >= 5 && mins <= 60) {
    _mesh->setNetsyncMaxwait(mins);
    sprintf(reply, "OK - Max wait set to %d minutes", mins);
  } else {
    strcpy(reply, "Error: Range 5-60 minutes");
  }
  return true;
}

bool FirmwareCLI::cmdGetNettimeMaxwait(char* reply) {
  sprintf(reply, "%d", _mesh->getNetsyncMaxwait());
  return true;
}

bool FirmwareCLI::cmdGetNettimeStatus(char* reply) {
  if (_mesh->isClockSynced() || !_mesh->isDesynced()) {
    strcpy(reply, "Clock already synced");
  } else if (!_mesh->isNetsyncEnabled()) {
    strcpy(reply, "Network time sync disabled");
  } else {
    sprintf(reply, "Waiting for repeaters (%d/3)", _mesh->getRepeaterCount());
  }
  return true;
}

bool FirmwareCLI::cmdGetChannelMode(char* reply) {
  strcpy(reply, _mesh->isChannelPrivate() ? "private" : "public");
  return true;
}

bool FirmwareCLI::cmdSetChannelMode(const char* val, char* reply) {
  if (strcmp(val, "public") == 0) {
    if (_mesh->isChannelPrivate()) {
      _mesh->setChannelModePublic();
    }
    strcpy(reply, "OK - Channel mode set to public");
  } else if (strcmp(val, "private") == 0) {
    if (!_mesh->isChannelPrivate()) {
      _mesh->setChannelModePrivate();
    }
    strcpy(reply, "OK - Channel mode set to private");
  } else {
    strcpy(reply, "Error: Use 'public' or 'private'");
  }
  return true;
}

bool FirmwareCLI::cmdLoginHistory(char* reply) {
  int count = _mesh->getLoginHistoryCount();
  if (count == 0) {
    strcpy(reply, "No login history available");
    return true;
  }

  reply[0] = '\0';
  sprintf(reply, "Last %d logins:\n", count);

  for (int i = 0; i < count; i++) {
    LoginHistoryEntry entry;
    if (_mesh->getLoginHistoryEntry(i, entry)) {
      const char* role = (entry.permissions == PERM_ACL_ADMIN) ? "admin" :
                         (entry.permissions == PERM_ACL_READ_WRITE) ? "user" : "guest";

      char timestamp_str[32];
      DateTime dt(entry.timestamp);
      sprintf(timestamp_str, "%02d/%02d/%04d %02d:%02d:%02d UTC",
              dt.day(), dt.month(), dt.year(),
              dt.hour(), dt.minute(), dt.second());

      char line[100];
      sprintf(line, "[%02X%02X%02X%02X] %s - %s\n",
              entry.pub_key[0], entry.pub_key[1], entry.pub_key[2], entry.pub_key[3],
              role, timestamp_str);
      strcat(reply, line);
    }
  }
  return true;
}

bool FirmwareCLI::cmdAppReply(const char* args, char* reply) {
  char* app_name = (char*)args;
  char* hex = strchr(app_name, ' ');
  if (hex == NULL) {
    strcpy(reply, "ERROR: Bad format. Use: appreply <app_name> <pubkey_hex> <response_text>");
    return true;
  }
  *hex++ = 0;

  char* response_text = strchr(hex, ' ');
  if (response_text == NULL) {
    strcpy(reply, "ERROR: Bad format. Use: appreply <app_name> <pubkey_hex> <response_text>");
    return true;
  }
  *response_text++ = 0;

  uint8_t pubkey[PUB_KEY_SIZE];
  int hex_len = strlen(hex);
  if (hex_len != PUB_KEY_SIZE * 2 || !mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) {
    strcpy(reply, "ERROR: Invalid pubkey hex");
    return true;
  }

  if (_mesh->sendAppReply(app_name, pubkey, response_text)) {
    strcpy(reply, "OK - Response sent");
  } else {
    strcpy(reply, "ERROR: Client not found");
  }
  return true;
}

bool FirmwareCLI::cmdEraseSDCard(char* reply) {
  SDStorage* sd = _mesh->getDataStore()->getSD();
  if (sd && sd->isReady()) {
    if (sd->eraseAllData()) {
      _mesh->backupConfigToSD();
      strcpy(reply, "OK - SD card data erased");
    } else {
      strcpy(reply, "ERROR: Erase failed");
    }
  } else {
    strcpy(reply, "ERROR: SD card not available");
  }
  return true;
}

bool FirmwareCLI::cmdGetSDStatus(char* reply) {
  SDStorage* sd = _mesh->getDataStore()->getSD();
  if (!sd) {
    strcpy(reply, "SD: Not supported (PIN_SDCARD_CS not defined)");
    return true;
  }

  SDStatus status = sd->getStatus();
  switch (status) {
    case SD_NOT_SUPPORTED:
      strcpy(reply, "SD: Not supported");
      break;
    case SD_NOT_PRESENT:
      strcpy(reply, "SD: No card detected");
      break;
    case SD_UNFORMATTED:
      strcpy(reply, "SD: Card unformatted or inaccessible");
      break;
    case SD_READY: {
      uint32_t used = sd->getUsedSpace();
      uint32_t total = sd->getTotalSpace();
      uint32_t free = sd->getFreeSpace();
      sprintf(reply, "SD: Ready - Used: %lu KB, Free: %lu KB, Total: %lu KB",
              (unsigned long)used, (unsigned long)free, (unsigned long)total);
      break;
    }
    default:
      strcpy(reply, "SD: Unknown status");
      break;
  }
  return true;
}
