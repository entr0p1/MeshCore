#pragma once

#include <Arduino.h>
#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#include "helpers/ClientACL.h"

#ifndef MAX_POST_TEXT_LEN
  #define MAX_POST_TEXT_LEN 140  // User message limit (prefix added on top)
#endif

#define MAX_SYSTEM_MESSAGES 8  // Keep a small queue of recent system messages

// System message with delivery tracking
struct SystemMessage {
  char text[MAX_POST_TEXT_LEN + 1];
  uint32_t boot_sequence;        // Boot number (for ordering across reboots)
  uint32_t created_millis;       // Millis since boot (for ordering within same boot)
  uint8_t delivered_to[MAX_CLIENTS * 6];  // 6-byte pub_key prefixes of admins who received this
};

// Persistent queue of system messages with per-admin delivery tracking
class SystemMessageHandler {
  SystemMessage messages[MAX_SYSTEM_MESSAGES];
  int num_messages;

public:
  SystemMessageHandler() {
    memset(messages, 0, sizeof(messages));
    num_messages = 0;
  }

  // Flash persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

  // Message management
  void addMessage(const char* text, uint32_t boot_seq);
  bool needsPush(int msg_idx, const ClientInfo* admin);
  void markPushed(int msg_idx, const ClientInfo* admin);
  void cleanup(const ClientACL* acl);

  // Accessors
  SystemMessage* getMessage(int idx) { return (idx < num_messages) ? &messages[idx] : NULL; }
  int getNumMessages() const { return num_messages; }
};
