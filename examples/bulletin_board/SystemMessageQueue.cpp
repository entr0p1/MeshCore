#include "SystemMessageQueue.h"
#include <helpers/TxtDataHelpers.h>

static File openWrite(FILESYSTEM* _fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(filename);
  return _fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "w");
#else
  return _fs->open(filename, "w", true);
#endif
}

void SystemMessageQueue::load(FILESYSTEM* fs) {
  num_messages = 0;

  if (!fs->exists("/system_msgs")) return;

#if defined(RP2040_PLATFORM)
  File file = fs->open("/system_msgs", "r");
#else
  File file = fs->open("/system_msgs");
#endif

  if (file) {
    file.read((uint8_t*)&num_messages, 1);

    for (int i = 0; i < num_messages && i < MAX_SYSTEM_MESSAGES; i++) {
      auto msg = &messages[i];
      file.read((uint8_t*)msg->text, sizeof(msg->text));
      file.read((uint8_t*)&msg->boot_sequence, 4);
      file.read((uint8_t*)&msg->created_millis, 4);
      file.read(msg->delivered_to, sizeof(msg->delivered_to));
    }

    file.close();
  }
}

void SystemMessageQueue::save(FILESYSTEM* fs) {
  File file = openWrite(fs, "/system_msgs");
  if (file) {
    file.write((uint8_t*)&num_messages, 1);

    for (int i = 0; i < num_messages; i++) {
      auto msg = &messages[i];
      file.write((uint8_t*)msg->text, sizeof(msg->text));
      file.write((uint8_t*)&msg->boot_sequence, 4);
      file.write((uint8_t*)&msg->created_millis, 4);
      file.write(msg->delivered_to, sizeof(msg->delivered_to));
    }

    file.close();
  }
}

void SystemMessageQueue::addMessage(const char* text, uint32_t boot_seq) {
  if (num_messages >= MAX_SYSTEM_MESSAGES) {
    // Evict oldest message (lowest boot_sequence, or lowest millis if same boot)
    int oldest_idx = 0;
    for (int i = 1; i < num_messages; i++) {
      if (messages[i].boot_sequence < messages[oldest_idx].boot_sequence ||
          (messages[i].boot_sequence == messages[oldest_idx].boot_sequence &&
           messages[i].created_millis < messages[oldest_idx].created_millis)) {
        oldest_idx = i;
      }
    }

    // Shift to remove oldest
    for (int i = oldest_idx; i < num_messages - 1; i++) {
      messages[i] = messages[i + 1];
    }
    num_messages--;
  }

  auto msg = &messages[num_messages];
  StrHelper::strncpy(msg->text, text, sizeof(msg->text));
  msg->boot_sequence = boot_seq;
  msg->created_millis = millis();
  memset(msg->delivered_to, 0, sizeof(msg->delivered_to));

  // Print the message creation to Serial with message ID
  Serial.printf("SystemMessageQueue: Message %d queued: %s\n", num_messages, text);

  num_messages++;
}

bool SystemMessageQueue::needsPush(int msg_idx, const ClientInfo* admin) {
  if (msg_idx >= num_messages) {
    MESH_DEBUG_PRINTLN("    needsPush: msg_idx %d >= num_messages %d", msg_idx, num_messages);
    return false;
  }
  if (!admin->isAdmin()) {
    MESH_DEBUG_PRINTLN("    needsPush: admin check failed");
    return false;
  }

  auto msg = &messages[msg_idx];
  MESH_DEBUG_PRINTLN("    needsPush[%d]: checking msg='%.30s...'", msg_idx, msg->text);

  // Check if this admin has already received this message
  for (int i = 0; i < MAX_CLIENTS; i++) {
    uint8_t* delivered_pubkey = &msg->delivered_to[i * 6];
    // Check if slot is occupied (non-zero) and matches this admin
    if ((delivered_pubkey[0] != 0 || delivered_pubkey[1] != 0) &&
        memcmp(delivered_pubkey, admin->id.pub_key, 6) == 0) {
      MESH_DEBUG_PRINTLN("    needsPush[%d]: ALREADY DELIVERED to %02X%02X%02X%02X",
                         msg_idx, admin->id.pub_key[0], admin->id.pub_key[1],
                         admin->id.pub_key[2], admin->id.pub_key[3]);
      return false;  // Already delivered to this admin
    }
  }

  MESH_DEBUG_PRINTLN("    needsPush[%d]: YES, needs push to %02X%02X%02X%02X",
                     msg_idx, admin->id.pub_key[0], admin->id.pub_key[1],
                     admin->id.pub_key[2], admin->id.pub_key[3]);
  return true;  // Not yet delivered
}

void SystemMessageQueue::markPushed(int msg_idx, const ClientInfo* admin) {
  if (msg_idx >= num_messages) return;

  auto msg = &messages[msg_idx];

  // Find empty slot and store pub_key prefix (first 6 bytes)
  for (int i = 0; i < MAX_CLIENTS; i++) {
    uint8_t* slot = &msg->delivered_to[i * 6];
    if (slot[0] == 0 && slot[1] == 0) {  // Empty slot (both first bytes zero)
      memcpy(slot, admin->id.pub_key, 6);
      return;
    }
  }
}

void SystemMessageQueue::cleanup(const ClientACL* acl) {
  // Remove messages that have been delivered to ALL current admins
  for (int i = 0; i < num_messages; ) {
    bool delivered_to_all = true;
    bool has_any_admins = false;

    // Check if every current admin has received this message
    for (int j = 0; j < acl->getNumClients(); j++) {
      auto admin = const_cast<ClientACL*>(acl)->getClientByIdx(j);
      if (admin->isAdmin()) {
        has_any_admins = true;

        bool found_in_delivered = false;
        auto msg = &messages[i];
        for (int k = 0; k < MAX_CLIENTS; k++) {
          uint8_t* delivered_pubkey = &msg->delivered_to[k * 6];
          if ((delivered_pubkey[0] != 0 || delivered_pubkey[1] != 0) &&
              memcmp(delivered_pubkey, admin->id.pub_key, 6) == 0) {
            found_in_delivered = true;
            break;
          }
        }

        if (!found_in_delivered) {
          delivered_to_all = false;
          break;
        }
      }
    }

    // If no admins exist, don't cleanup yet - wait for admins to connect
    if (!has_any_admins) {
      delivered_to_all = false;
    }

    if (delivered_to_all) {
      // Remove this message (shift array left)
      for (int k = i; k < num_messages - 1; k++) {
        messages[k] = messages[k + 1];
      }
      num_messages--;
      // Don't increment i - check same index again
    } else {
      i++;  // Check next message
    }
  }
}
