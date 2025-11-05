#include "MyMesh.h"
#include "SystemMessageQueue.h"

#ifdef DISPLAY_CLASS
#include "UITask.h"
extern UITask ui_task;
#endif

#define REPLY_DELAY_MILLIS          1500
#define PUSH_NOTIFY_DELAY_MILLIS    2000
#define SYNC_PUSH_INTERVAL          1200

#define PUSH_ACK_TIMEOUT_FLOOD      12000
#define PUSH_TIMEOUT_BASE           4000
#define PUSH_ACK_TIMEOUT_FACTOR     2000

#define POST_SYNC_DELAY_SECS        6

#define FIRMWARE_VER_LEVEL       1

#define MIN_VALID_TIMESTAMP 1735689600  // Jan 1, 2025 00:00:00 UTC

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define LAZY_CONTACTS_WRITE_DELAY    5000

struct ServerStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t noise_floor;
  int16_t last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events; // was 'n_full_events'
  int16_t last_snr;    // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint16_t n_posted, n_post_push;
};

void MyMesh::addPost(ClientInfo *client, const char *postData) {
  // TODO: suggested postData format: <title>/<descrption>
  posts[next_post_idx].author = client->id; // add to cyclic queue
  StrHelper::strncpy(posts[next_post_idx].text, postData, MAX_POST_TEXT_LEN);

  posts[next_post_idx].post_timestamp = getRTCClock()->getCurrentTimeUnique();
  next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;

  next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _num_posted++; // stats

  // Save posts to flash
  savePosts();
}

void MyMesh::addBulletin(const char* bulletinText) {
  // Length validation - abort if too long
  if (strlen(bulletinText) > MAX_POST_TEXT_LEN) {
    return; // Abort - don't post anything
  }

  posts[next_post_idx].author = self_id;  // Use server's identity
  StrHelper::strncpy(posts[next_post_idx].text, bulletinText, MAX_POST_TEXT_LEN);

  posts[next_post_idx].post_timestamp = getRTCClock()->getCurrentTimeUnique();
  next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;

  next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _num_posted++; // stats

  // Save posts to flash
  savePosts();
}

void MyMesh::pushPostToClient(ClientInfo *client, PostInfo &post) {
  int len = 0;
  memcpy(&reply_data[len], &post.post_timestamp, 4);
  len += 4; // this is a PAST timestamp... but should be accepted by client

  uint8_t attempt;
  getRNG()->random(&attempt, 1); // need this for re-tries, so packet hash (and ACK) will be different
  reply_data[len++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3); // 'signed' plain text

  // encode prefix of post.author.pub_key
  memcpy(&reply_data[len], post.author.pub_key, 4);
  len += 4; // just first 4 bytes

  int text_len = strlen(post.text);
  memcpy(&reply_data[len], post.text, text_len);
  len += text_len;

  // calc expected ACK reply
  mesh::Utils::sha256((uint8_t *)&client->extra.room.pending_ack, 4, reply_data, len, client->id.pub_key, PUB_KEY_SIZE);
  client->extra.room.push_post_timestamp = post.post_timestamp;

  auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, reply_data, len);
  if (reply) {
    if (client->out_path_len < 0) {
      sendFlood(reply);
      client->extra.room.ack_timeout = futureMillis(PUSH_ACK_TIMEOUT_FLOOD);
    } else {
      sendDirect(reply, client->out_path, client->out_path_len);
      client->extra.room.ack_timeout =
          futureMillis(PUSH_TIMEOUT_BASE + PUSH_ACK_TIMEOUT_FACTOR * (client->out_path_len + 1));
    }
    _num_post_pushes++; // stats
  } else {
    client->extra.room.pending_ack = 0;
    MESH_DEBUG_PRINTLN("Unable to push post to client");
  }
}

uint8_t MyMesh::getUnsyncedCount(ClientInfo *client) {
  uint8_t count = 0;
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (posts[k].post_timestamp > client->extra.room.sync_since // is new post for this Client?
        && !posts[k].author.matches(client->id)) {   // don't push posts to the author
      count++;
    }
  }
  return count;
}

bool MyMesh::processAck(const uint8_t *data) {
  for (int i = 0; i < acl.getNumClients(); i++) {
    auto client = acl.getClientByIdx(i);
    if (client->extra.room.pending_ack && memcmp(data, &client->extra.room.pending_ack, 4) == 0) { // got an ACK from Client!
      client->extra.room.pending_ack = 0; // clear this, so next push can happen
      client->extra.room.push_failures = 0;
      client->extra.room.sync_since = client->extra.room.push_post_timestamp; // advance Client's SINCE timestamp, to sync next post

      // If this was a system message, mark it as delivered now that ACK received
      if (pending_system_msg_idx[i] >= 0) {
        int msg_idx = pending_system_msg_idx[i];
        system_msgs->markPushed(msg_idx, client);
        system_msgs->save(_fs);
        MESH_DEBUG_PRINTLN("System message %d ACKed by admin %02X, marked delivered", msg_idx, (uint32_t)client->id.pub_key[0]);

        // Always-on Serial output for production monitoring
        Serial.printf("SystemMessageQueue: Message %d delivered to admin [%02X%02X%02X%02X]\n",
                      msg_idx,
                      (uint32_t)client->id.pub_key[0],
                      (uint32_t)client->id.pub_key[1],
                      (uint32_t)client->id.pub_key[2],
                      (uint32_t)client->id.pub_key[3]);

        // Reset pre-login attempts for this message since it was successfully delivered
        system_msg_prelogin_attempts[i][msg_idx] = 0;

        pending_system_msg_idx[i] = -1;  // Clear pending
      }

      return true;
    }
  }
  return false;
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len;
  {
    AdvertDataBuilder builder(ADV_TYPE_ROOM, _prefs.node_name, _prefs.node_lat, _prefs.node_lon);
    app_data_len = builder.encodeTo(app_data);
  }

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

File MyMesh::openFileForWrite(const char *filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(filename);  // Delete before write on NRF52/STM32
  return _fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "w");
#else
  return _fs->open(filename, "w", true);
#endif
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload,
                          size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {
    ServerStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundCount(0xFFFFFFFF);
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = _ms->getMillis() / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.n_posted = _num_posted;
    stats.n_post_push = _num_post_pushes;

    memcpy(&reply_data[4], &stats, sizeof(stats));
    return 4 + sizeof(stats);
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
    // query other sensors -- target specific
    sensors.querySensors((sender->isAdmin() ? 0xFF : 0x00) & perm_mask, telemetry);

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (!c->isAdmin()) continue;  // skip non-Admin entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  return 0; // unknown command
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}
void MyMesh::logTx(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}
void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 6) * t;
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->path_len + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 6) * t;
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->path_len >= _prefs.flood_max) return false;
  return true;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t sender_timestamp, sender_sync_since;
    memcpy(&sender_timestamp, data, 4);
    memcpy(&sender_sync_since, &data[4], 4); // sender's "sync messags SINCE x" timestamp

    data[len] = 0;                                        // ensure null terminator

    ClientInfo* client = NULL;
    if (data[8] == 0 && !_prefs.allow_read_only) {   // blank password, just check if sender is in ACL
      client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
      if (client == NULL) {
      #if MESH_DEBUG
        MESH_DEBUG_PRINTLN("Login, sender not in ACL");
      #endif
      }
    }
    if (client == NULL) {
      uint8_t perm;
      if (strcmp((char *)&data[8], _prefs.password) == 0) { // check for valid admin password
        perm = PERM_ACL_ADMIN;
      } else {
        if (strcmp((char *)&data[8], _prefs.guest_password) == 0) {   // check the room/public password
          perm = PERM_ACL_READ_WRITE;
        } else if (_prefs.allow_read_only) {
          perm = PERM_ACL_GUEST;
        } else {
          MESH_DEBUG_PRINTLN("Incorrect room password");
          return; // no response. Client will timeout
        }
      }

      // Clock synchronisation from admin login
      MESH_DEBUG_PRINTLN("Login: perm=%d, isDesynced=%d, clock_synced_once=%d, sender_ts=%lu",
                         perm, isDesynced(), clock_synced_once, sender_timestamp);
      if (perm == PERM_ACL_ADMIN && isDesynced() && !clock_synced_once) {
        if (sender_timestamp >= MIN_VALID_TIMESTAMP) {
          getRTCClock()->setCurrentTime(sender_timestamp);
          notifyClockSynced(sender.pub_key);

          // Clear repeater buffer - admin sync takes precedence
          repeater_count = 0;
          check_netsync_flag = false;

          MESH_DEBUG_PRINTLN("Clock synced from admin login %02X%02X: %lu",
                            sender.pub_key[0], sender.pub_key[1],
                            sender_timestamp);
        } else {
          MESH_DEBUG_PRINTLN("Admin login but timestamp %lu < MIN_VALID %lu", sender_timestamp, MIN_VALID_TIMESTAMP);
        }
      }

      client = acl.putClient(sender, 0);  // add to known clients (if not already known)
      if (sender_timestamp <= client->last_timestamp) {
        MESH_DEBUG_PRINTLN("possible replay attack!");
        return;
      }

      MESH_DEBUG_PRINTLN("Login success!");
      client->last_timestamp = sender_timestamp;
      client->extra.room.sync_since = sender_sync_since;
      client->extra.room.pending_ack = 0;
      client->extra.room.push_failures = 0;

      client->last_activity = getRTCClock()->getCurrentTime();
      client->permissions |= perm;
      memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

      // Print login message for all users (with role)
      const char* role = client->isAdmin() ? "admin" : "user";
      Serial.printf("MyMesh: User login: [%02X%02X%02X%02X] (%s)\n",
                    (uint32_t)client->id.pub_key[0],
                    (uint32_t)client->id.pub_key[1],
                    (uint32_t)client->id.pub_key[2],
                    (uint32_t)client->id.pub_key[3],
                    role);

      // Reset pre-login attempts for all system messages when admin logs in
      // (they're now active, so normal delivery tracking applies)
      int client_idx = -1;
      for (int i = 0; i < acl.getNumClients(); i++) {
        if (acl.getClientByIdx(i) == client) {
          client_idx = i;
          break;
        }
      }
      if (client_idx >= 0 && client->isAdmin()) {
        memset(system_msg_prelogin_attempts[client_idx], 0, sizeof(system_msg_prelogin_attempts[client_idx]));
        MESH_DEBUG_PRINTLN("Admin %02X logged in, reset pre-login attempts", (uint32_t)client->id.pub_key[0]);
        // Note: Delivery attempts will be logged as they occur in the main loop
      }

      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }

    uint32_t now = getRTCClock()->getCurrentTimeUnique();
    memcpy(reply_data, &now, 4); // response packets always prefixed with timestamp
    // TODO: maybe reply with count of messages waiting to be synced for THIS client?
    reply_data[4] = RESP_SERVER_LOGIN_OK;
    reply_data[5] = 0; // Legacy: was recommended keep-alive interval (secs / 16)
    reply_data[6] = (client->isAdmin() ? 1 : (client->permissions == 0 ? 2 : 0));
    // LEGACY: reply_data[7] = getUnsyncedCount(client);
    reply_data[7] = client->permissions; // NEW
    getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
    reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

    next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS); // delay next push, give RESPONSE packet time to arrive first

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet *path = createPathReturn(sender, client->shared_secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, 13);
      if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
    } else {
      mesh::Packet *reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, client->shared_secret, reply_data, 13);
      if (reply) {
        if (client->out_path_len >= 0) { // we have an out_path, so send DIRECT
          sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
        } else {
          sendFlood(reply, SERVER_RESPONSE_DELAY);
        }
      }
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  auto client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) { // a CLI command or new Post
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint flags = (data[4] >> 2);        // message attempt number, and other flags

    // Clock synchronisation logic - sync from first admin packet with valid timestamp
    if (isDesynced() && !clock_synced_once && client->isAdmin()) {
      if (sender_timestamp >= MIN_VALID_TIMESTAMP) {
        getRTCClock()->setCurrentTime(sender_timestamp);
        notifyClockSynced(client->id.pub_key);

        // Clear repeater buffer - admin sync takes precedence
        repeater_count = 0;
        check_netsync_flag = false;

        MESH_DEBUG_PRINTLN("Clock synced from admin %02X%02X: %lu",
                          client->id.pub_key[0], client->id.pub_key[1],
                          sender_timestamp);
      }
    }

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported command flags received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks, but send Acks for retries
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;

      uint32_t now = getRTCClock()->getCurrentTimeUnique();
      client->last_activity = now;
      client->extra.room.push_failures = 0; // reset so push can resume (if prev failed)

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove to
                         // sender that we got it
      mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                          PUB_KEY_SIZE);

      uint8_t temp[166];
      bool send_ack;
      if (flags == TXT_TYPE_CLI_DATA) {
        if (client->isAdmin()) {
          if (is_retry) {
            temp[5] = 0; // no reply
          } else {
            handleCommand(sender_timestamp, (char *)&data[5], (char *)&temp[5]);
            temp[4] = (TXT_TYPE_CLI_DATA << 2); // attempt and flags,  (NOTE: legacy was: TXT_TYPE_PLAIN)
          }
          send_ack = false;
        } else {
          temp[5] = 0;      // no reply
          send_ack = false; // and no ACK...  user shoudn't be sending these
        }
      } else { // TXT_TYPE_PLAIN
        if ((client->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
          temp[5] = 0;      // no reply
          send_ack = false; // no ACK
        } else {
          // Block posting if clock is desynced
          if (isDesynced()) {
            strcpy((char*)&temp[5], "Error: Server clock desynced");
            temp[4] = (TXT_TYPE_CLI_DATA << 2); // Send error as CLI_DATA
            send_ack = false; // No ACK for error
          } else {
            if (!is_retry) {
              addPost(client, (const char *)&data[5]);
            }
            temp[5] = 0; // no reply (ACK is enough)
            send_ack = true;
          }
        }
      }

      uint32_t delay_millis;
      if (send_ack) {
        if (client->out_path_len < 0) {
          mesh::Packet *ack = createAck(ack_hash);
          if (ack) sendFlood(ack, TXT_ACK_DELAY);
          delay_millis = TXT_ACK_DELAY + REPLY_DELAY_MILLIS;
        } else {
          uint32_t d = TXT_ACK_DELAY;
          if (getExtraAckTransmitCount() > 0) {
            mesh::Packet *a1 = createMultiAck(ack_hash, 1);
            if (a1) sendDirect(a1, client->out_path, client->out_path_len, d);
            d += 300;
          }

          mesh::Packet *a2 = createAck(ack_hash);
          if (a2) sendDirect(a2, client->out_path, client->out_path_len, d);
          delay_millis = d + REPLY_DELAY_MILLIS;
        }
      } else {
        delay_millis = 0;
      }

      int text_len = strlen((char *)&temp[5]);
      if (text_len > 0) {
        if (now == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          now++;
        }
        memcpy(temp, &now, 4); // mostly an extra blob to help make packet_hash unique

        // calc expected ACK reply
        // mesh::Utils::sha256((uint8_t *)&expected_ack_crc, 4, temp, 5 + text_len, self_id.pub_key,
        // PUB_KEY_SIZE);

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len < 0) {
            sendFlood(reply, delay_millis + SERVER_RESPONSE_DELAY);
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, delay_millis + SERVER_RESPONSE_DELAY);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_REQ && len >= 5) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    if (sender_timestamp < client->last_timestamp) { // prevent replay attacks
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    } else {
      client->last_timestamp = sender_timestamp;

      uint32_t now = getRTCClock()->getCurrentTime();
      client->last_activity = now; // <-- THIS will keep client connection alive
      client->extra.room.push_failures = 0;   // reset so push can resume (if prev failed)

      if (data[4] == REQ_TYPE_KEEP_ALIVE && packet->isRouteDirect()) { // request type
        uint32_t forceSince = 0;
        if (len >= 9) {                     // optional - last post_timestamp client received
          memcpy(&forceSince, &data[5], 4); // NOTE: this may be 0, if part of decrypted PADDING!
        } else {
          memcpy(&data[5], &forceSince, 4); // make sure there are zeroes in payload (for ack_hash calc below)
        }
        if (forceSince > 0) {
          client->extra.room.sync_since = forceSince; // force-update the 'sync since'
        }

        client->extra.room.pending_ack = 0;

        // TODO: Throttle KEEP_ALIVE requests!
        // if client sends too quickly, evict()

        // RULE: only send keep_alive response DIRECT!
        if (client->out_path_len >= 0) {
          uint32_t ack_hash; // calc ACK to prove to sender that we got request
          mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 9, client->id.pub_key, PUB_KEY_SIZE);

          auto reply = createAck(ack_hash);
          if (reply) {
            reply->payload[reply->payload_len++] = getUnsyncedCount(client); // NEW: add unsynced counter to end of ACK packet
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          }
        }
      } else {
        int reply_len = handleRequest(client, sender_timestamp, &data[4], len - 4);
        if (reply_len > 0) { // valid command
          if (packet->isRouteFlood()) {
            // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
            mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                                  PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
            if (path) sendFlood(path, SERVER_RESPONSE_DELAY);
          } else {
            mesh::Packet *reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
            if (reply) {
              if (client->out_path_len >= 0) { // we have an out_path, so send DIRECT
                sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
              } else {
                sendFlood(reply, SERVER_RESPONSE_DELAY);
              }
            }
          }
        }
      }
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);
    memcpy(client->out_path, path, client->out_path_len = path_len); // store a copy of path, for sendDirect()
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) {
    // also got an encoded ACK!
    processAck(extra);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

void MyMesh::onAckRecv(mesh::Packet *packet, uint32_t ack_crc) {
  if (processAck((uint8_t *)&ack_crc)) {
    packet->markDoNotRetransmit(); // ACK was for this node, so don't retransmit
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cli(board, rtc, &_prefs, this), telemetry(MAX_PACKET_PAYLOAD - 4) {
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  _logging = false;
  set_radio_at = revert_radio_at = 0;
  current_boot_sequence = 0;
  system_msgs = new SystemMessageQueue();
  clock_synced_once = false;

  // Network time sync initialisation
  repeater_count = 0;
  check_netsync_flag = false;
  memset(&netsync_config, 0, sizeof(netsync_config));
  memset(repeater_buffer, 0, sizeof(repeater_buffer));
  netsync_config.enabled = 0;         // Default: disabled (admin sync preferred)
  netsync_config.maxwait_mins = 15;   // Default: 15 minutes
  netsync_config.guard = 0xDEADBEEF;  // Validation marker

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;   // one half
  _prefs.rx_delay_base = 0.0f;   // off by default, was 10.0
  _prefs.tx_delay_factor = 0.5f; // was 0.25f;
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.disable_fwd = 1;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 12; // 12 hours
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0; // disabled
#ifdef ROOM_PASSWORD
  StrHelper::strncpy(_prefs.guest_password, ROOM_PASSWORD, sizeof(_prefs.guest_password));
#endif

  next_post_idx = 0;
  next_client_idx = 0;
  next_push = 0;
  memset(posts, 0, sizeof(posts));
  memset(pending_system_msg_idx, -1, sizeof(pending_system_msg_idx));  // -1 = no pending system message
  memset(system_msg_prelogin_attempts, 0, sizeof(system_msg_prelogin_attempts));  // 0 = no attempts yet
  _num_posted = _num_post_pushes = 0;
}

uint32_t MyMesh::loadBootCounter(FILESYSTEM* fs) {
  if (!fs->exists("/boot_count")) return 0;

#if defined(RP2040_PLATFORM)
  File file = fs->open("/boot_count", "r");
#else
  File file = fs->open("/boot_count");
#endif

  if (file) {
    uint32_t count = 0;
    file.read((uint8_t*)&count, 4);
    file.close();
    return count;
  }
  return 0;
}

void MyMesh::saveBootCounter(FILESYSTEM* fs, uint32_t count) {
  File file = openFileForWrite("/boot_count");

  if (file) {
    file.write((uint8_t*)&count, 4);
    file.close();
  }
}

bool MyMesh::isDesynced() const {
  return getRTCClock()->getCurrentTime() < MIN_VALID_TIMESTAMP;
}

void MyMesh::notifyClockSynced(const uint8_t* admin_pubkey) {
  if (!clock_synced_once) {
    clock_synced_once = true;

    // Add system message announcing successful sync
    char sync_msg[MAX_POST_TEXT_LEN + 1];
    if (admin_pubkey) {
      sprintf(sync_msg, "Clock synced by admin [%02X%02X%02X%02X]. Server now in read-write mode.",
              admin_pubkey[0], admin_pubkey[1], admin_pubkey[2], admin_pubkey[3]);
    } else {
      strcpy(sync_msg, "Clock synced manually. Server now in read-write mode.");
    }
    addSystemMessage(sync_msg);
  }
}

void MyMesh::addSystemMessage(const char* message) {
  // Format: "SYSTEM: boot:[n] msg:[user message]"
  // This ensures each boot's messages are unique for companion app deduplication
  char formatted_msg[MAX_POST_TEXT_LEN + 1];
  snprintf(formatted_msg, sizeof(formatted_msg), "SYSTEM: boot:%u msg:%s",
           current_boot_sequence, message);

  system_msgs->addMessage(formatted_msg, current_boot_sequence);
  system_msgs->save(_fs);
  // Note: Serial output now handled by SystemMessageQueue::addMessage()
  MESH_DEBUG_PRINTLN("Added system message (boot %u), now have %d messages",
                     current_boot_sequence, system_msgs->getNumMessages());
}

// ----------------------- Network Time Synchronisation -----------------------

void MyMesh::loadNetSyncConfig() {
  File f = _fs->open("/netsync_cfg", "r");
  if (f) {
    ClockNetSyncConfig loaded_config;
    size_t bytes_read = f.read((uint8_t*)&loaded_config, sizeof(loaded_config));
    f.close();

    // Validate and apply
    if (bytes_read == sizeof(loaded_config) && loaded_config.guard == 0xDEADBEEF) {
      // Range check maxwait_mins
      if (loaded_config.maxwait_mins >= 5 && loaded_config.maxwait_mins <= 60) {
        netsync_config = loaded_config;
        MESH_DEBUG_PRINTLN("Loaded network time sync config: enabled=%d, maxwait=%d min",
                          netsync_config.enabled, netsync_config.maxwait_mins);
      } else {
        MESH_DEBUG_PRINTLN("Invalid maxwait_mins in config, using defaults");
      }
    } else {
      MESH_DEBUG_PRINTLN("Invalid network time sync config, using defaults");
    }
  } else {
    MESH_DEBUG_PRINTLN("No network time sync config found, using defaults");
  }
}

void MyMesh::saveNetSyncConfig() {
  File f = openFileForWrite("/netsync_cfg");
  if (f) {
    f.write((uint8_t*)&netsync_config, sizeof(netsync_config));
    f.close();
    MESH_DEBUG_PRINTLN("Saved network time sync config");
  }
}

void MyMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                          const uint8_t* app_data, size_t app_data_len) {
  // Only process if clock not synced yet
  if (clock_synced_once || !isDesynced() || !netsync_config.enabled) {
    return;  // Already synced, desynced but functional, or feature disabled
  }

  // Parse advert type
  AdvertDataParser parser(app_data, app_data_len);
  if (parser.getType() != ADV_TYPE_REPEATER) {
    return;  // Not a repeater advert
  }

  // Validate timestamp
  if (timestamp < MIN_VALID_TIMESTAMP) {
    MESH_DEBUG_PRINTLN("Repeater advert has invalid timestamp %lu < %lu", timestamp, MIN_VALID_TIMESTAMP);
    return;  // Too old
  }

  // Check if we already have this repeater (by first 4 bytes of pub_key)
  bool already_stored = false;
  for (int i = 0; i < repeater_count; i++) {
    if (memcmp(repeater_buffer[i].pub_key, id.pub_key, 4) == 0) {
      // Update timestamp from same repeater (take newer)
      if (timestamp > repeater_buffer[i].timestamp) {
        repeater_buffer[i].timestamp = timestamp;
        repeater_buffer[i].received_time = getRTCClock()->getCurrentTime();
        MESH_DEBUG_PRINTLN("Updated repeater [%02X%02X%02X%02X] timestamp to %lu",
                          id.pub_key[0], id.pub_key[1], id.pub_key[2], id.pub_key[3], timestamp);
      }
      already_stored = true;
      break;
    }
  }

  if (!already_stored) {
    // Add new repeater
    if (repeater_count < 3) {
      memcpy(repeater_buffer[repeater_count].pub_key, id.pub_key, 4);
      repeater_buffer[repeater_count].timestamp = timestamp;
      repeater_buffer[repeater_count].received_time = getRTCClock()->getCurrentTime();
      repeater_count++;
      MESH_DEBUG_PRINTLN("Added repeater [%02X%02X%02X%02X] to buffer (count=%d/3), timestamp=%lu",
                        id.pub_key[0], id.pub_key[1], id.pub_key[2], id.pub_key[3],
                        repeater_count, timestamp);
    } else {
      // Buffer full - replace oldest by received_time
      int oldest_idx = 0;
      uint32_t oldest_time = repeater_buffer[0].received_time;
      for (int i = 1; i < 3; i++) {
        if (repeater_buffer[i].received_time < oldest_time) {
          oldest_time = repeater_buffer[i].received_time;
          oldest_idx = i;
        }
      }
      MESH_DEBUG_PRINTLN("Buffer full, replacing oldest repeater [%02X%02X%02X%02X]",
                        repeater_buffer[oldest_idx].pub_key[0], repeater_buffer[oldest_idx].pub_key[1],
                        repeater_buffer[oldest_idx].pub_key[2], repeater_buffer[oldest_idx].pub_key[3]);
      memcpy(repeater_buffer[oldest_idx].pub_key, id.pub_key, 4);
      repeater_buffer[oldest_idx].timestamp = timestamp;
      repeater_buffer[oldest_idx].received_time = getRTCClock()->getCurrentTime();
    }

    check_netsync_flag = true;  // Signal to check for sync
  }
}

void MyMesh::checkNetworkTimeSync() {
  if (!check_netsync_flag) return;
  check_netsync_flag = false;

  if (clock_synced_once || !isDesynced() || !netsync_config.enabled) {
    return;  // Already synced, desynced but functional, or feature disabled
  }

  if (repeater_count < 3) {
    return;  // Need at least 3
  }

  // Age out old adverts (older than maxwait from current time)
  uint32_t now = getRTCClock()->getCurrentTime();
  uint32_t maxwait_secs = netsync_config.maxwait_mins * 60;

  for (int i = 0; i < repeater_count; ) {
    // Note: Our clock might be wrong (year 2000), so this aging might not work correctly
    // However, once we get close to syncing, it should be more accurate
    // The agreement check below is the primary filter
    if (now > MIN_VALID_TIMESTAMP && now > repeater_buffer[i].received_time + maxwait_secs) {
      // Remove old advert (shift array)
      MESH_DEBUG_PRINTLN("Aging out old repeater advert [%02X%02X%02X%02X]",
                        repeater_buffer[i].pub_key[0], repeater_buffer[i].pub_key[1],
                        repeater_buffer[i].pub_key[2], repeater_buffer[i].pub_key[3]);
      for (int j = i; j < repeater_count - 1; j++) {
        repeater_buffer[j] = repeater_buffer[j + 1];
      }
      repeater_count--;
    } else {
      i++;
    }
  }

  if (repeater_count < 3) {
    MESH_DEBUG_PRINTLN("After aging, only %d/3 repeaters remain", repeater_count);
    return;  // After aging, not enough left
  }

  // Check: All timestamps within maxwait of each other?
  uint32_t min_ts = repeater_buffer[0].timestamp;
  uint32_t max_ts = repeater_buffer[0].timestamp;
  int most_recent_idx = 0;

  for (int i = 1; i < repeater_count; i++) {
    if (repeater_buffer[i].timestamp < min_ts) min_ts = repeater_buffer[i].timestamp;
    if (repeater_buffer[i].timestamp > max_ts) {
      max_ts = repeater_buffer[i].timestamp;
      most_recent_idx = i;
    }
  }

  uint32_t span_secs = max_ts - min_ts;
  MESH_DEBUG_PRINTLN("Timestamp span: %lu seconds (max=%lu, min=%lu, maxwait=%lu)",
                    span_secs, max_ts, min_ts, maxwait_secs);

  if (span_secs > maxwait_secs) {
    // Disagreement too large - discard oldest by received_time and wait
    int oldest_idx = 0;
    uint32_t oldest_time = repeater_buffer[0].received_time;
    for (int i = 1; i < repeater_count; i++) {
      if (repeater_buffer[i].received_time < oldest_time) {
        oldest_time = repeater_buffer[i].received_time;
        oldest_idx = i;
      }
    }

    MESH_DEBUG_PRINTLN("Span exceeds maxwait, discarding oldest repeater [%02X%02X%02X%02X]",
                      repeater_buffer[oldest_idx].pub_key[0], repeater_buffer[oldest_idx].pub_key[1],
                      repeater_buffer[oldest_idx].pub_key[2], repeater_buffer[oldest_idx].pub_key[3]);

    // Remove oldest
    for (int j = oldest_idx; j < repeater_count - 1; j++) {
      repeater_buffer[j] = repeater_buffer[j + 1];
    }
    repeater_count--;
    return;  // Wait for next advert
  }

  // All agree! Use most recent timestamp
  uint32_t sync_timestamp = repeater_buffer[most_recent_idx].timestamp;

  // Monotonic check: timestamp should be > our current time
  // (If our clock is year 2000, this will pass. If our clock is already reasonable, verify forward movement)
  if (now > MIN_VALID_TIMESTAMP && sync_timestamp <= now) {
    // Timestamp is in past or present - probably wrong, discard all and restart
    MESH_DEBUG_PRINTLN("Most recent timestamp %lu <= current time %lu, discarding all and restarting",
                      sync_timestamp, now);
    repeater_count = 0;
    return;
  }

  // Sync!
  MESH_DEBUG_PRINTLN("Network time sync: Setting clock to %lu from repeater [%02X%02X%02X%02X]",
                    sync_timestamp,
                    repeater_buffer[most_recent_idx].pub_key[0],
                    repeater_buffer[most_recent_idx].pub_key[1],
                    repeater_buffer[most_recent_idx].pub_key[2],
                    repeater_buffer[most_recent_idx].pub_key[3]);

  getRTCClock()->setCurrentTime(sync_timestamp);
  clock_synced_once = true;

  // Create system message with details
  notifyClockSyncedFromRepeaters();

  // Clear buffer
  repeater_count = 0;
}

void MyMesh::notifyClockSyncedFromRepeaters() {
  char msg[MAX_POST_TEXT_LEN + 1];
  char temp[80];

  // Find most recent timestamp and its repeater
  int most_recent_idx = 0;
  uint32_t max_ts = repeater_buffer[0].timestamp;
  for (int i = 1; i < repeater_count; i++) {
    if (repeater_buffer[i].timestamp > max_ts) {
      max_ts = repeater_buffer[i].timestamp;
      most_recent_idx = i;
    }
  }

  // Format: "Clock set by Repeater advert from [AABBCCDD] to 01 Jan 2025 10:30. Quorum nodes: [AAAAAAAA], [BBBBBBBB], [CCCCCCCC]."
  DateTime dt(max_ts);
  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  sprintf(msg, "Clock set by Repeater advert from [%02X%02X%02X%02X] to %02d %s %04d %02d:%02d. Quorum nodes: ",
          repeater_buffer[most_recent_idx].pub_key[0],
          repeater_buffer[most_recent_idx].pub_key[1],
          repeater_buffer[most_recent_idx].pub_key[2],
          repeater_buffer[most_recent_idx].pub_key[3],
          dt.day(),
          months[dt.month() - 1],
          dt.year(),
          dt.hour(),
          dt.minute());

  // Add all repeater IDs to quorum list
  for (int i = 0; i < repeater_count && i < 3; i++) {
    sprintf(temp, "[%02X%02X%02X%02X]",
            repeater_buffer[i].pub_key[0],
            repeater_buffer[i].pub_key[1],
            repeater_buffer[i].pub_key[2],
            repeater_buffer[i].pub_key[3]);
    strcat(msg, temp);
    if (i < repeater_count - 1) strcat(msg, ", ");
  }
  strcat(msg, ".");

  addSystemMessage(msg);
}

// ----------------------------------------------------------------------------

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;

  // Load and increment boot counter
  current_boot_sequence = loadBootCounter(_fs);
  current_boot_sequence++;
  saveBootCounter(_fs, current_boot_sequence);

  // load persisted prefs
  _cli.loadPrefs(_fs);

  acl.load(_fs);

  // Load persisted posts
  loadPosts();

  // Load system message queue
  system_msgs->load(_fs);
  MESH_DEBUG_PRINTLN("Loaded %d system messages from flash", system_msgs->getNumMessages());

  // Load network time sync configuration
  loadNetSyncConfig();

  // Wait 5 seconds for Serial console to initialise before creating system messages
  MESH_DEBUG_PRINTLN("Waiting 5 seconds for Serial console initialisation...");
  delay(5000);

  // Check current clock state
  uint32_t current_time = getRTCClock()->getCurrentTime();
  MESH_DEBUG_PRINTLN("RTC current_time=%lu, MIN_VALID=%lu, isDesynced=%d",
                     current_time, MIN_VALID_TIMESTAMP, isDesynced());

  // If clock is desynced, add system message
  if (isDesynced()) {
    addSystemMessage("Server rebooted. Clock desynced - read-only until admin login.");
  }

  radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_set_tx_power(_prefs.tx_power_dbm);

  updateAdvertTimer();
  updateFloodAdvertTimer();
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    sendFlood(pkt, delay_millis);
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}
void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(uint8_t power_dbm) {
  radio_set_tx_power(power_dbm);
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
  self_id = new_id;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", self_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  while (*command == ' ')
    command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (sender_timestamp == 0 && memcmp(command, "addbulletin ", 12) == 0) {
    // Serial-only command (server admin can post bulletins)
    if (isDesynced()) {
      strcpy(reply, "Error: Clock not synced");
    } else {
      const char* bulletin_text = &command[12];
      int text_len = strlen(bulletin_text);

      if (text_len == 0) {
        strcpy(reply, "Error: Empty bulletin");
      } else if (text_len > MAX_POST_TEXT_LEN) {
        sprintf(reply, "Error: Max %d chars", MAX_POST_TEXT_LEN);
      } else {
        addBulletin(bulletin_text);
        strcpy(reply, "Bulletin added");
      }
    }
  }
  // Network time sync CLI commands
  else if (memcmp(command, "set nettime.enable ", 19) == 0) {
    const char* val = &command[19];
    if (strcmp(val, "on") == 0) {
      netsync_config.enabled = 1;
      saveNetSyncConfig();
      strcpy(reply, "OK - Network time sync enabled");
    } else if (strcmp(val, "off") == 0) {
      netsync_config.enabled = 0;
      saveNetSyncConfig();
      strcpy(reply, "OK - Network time sync disabled");
    } else {
      strcpy(reply, "Error: Use 'on' or 'off'");
    }
  } else if (strcmp(command, "get nettime.enable") == 0) {
    strcpy(reply, netsync_config.enabled ? "on" : "off");
  } else if (memcmp(command, "set nettime.maxwait ", 20) == 0) {
    int mins = atoi(&command[20]);
    if (mins >= 5 && mins <= 60) {
      netsync_config.maxwait_mins = mins;
      saveNetSyncConfig();
      sprintf(reply, "OK - Max wait set to %d minutes", mins);
    } else {
      strcpy(reply, "Error: Range 5-60 minutes");
    }
  } else if (strcmp(command, "get nettime.maxwait") == 0) {
    sprintf(reply, "%d", netsync_config.maxwait_mins);
  } else if (strcmp(command, "get nettime.status") == 0) {
    if (clock_synced_once || !isDesynced()) {
      strcpy(reply, "Clock already synced");
    } else if (!netsync_config.enabled) {
      strcpy(reply, "Network time sync disabled");
    } else {
      sprintf(reply, "Waiting for repeaters (%d/3)", repeater_count);
    }
  } else {
    bool was_desynced = isDesynced();
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands

    // If clock was desynced and now isn't, notify clock sync
    if (was_desynced && !isDesynced()) {
      notifyClockSynced(NULL);  // NULL = manual sync via CLI
    }
  }
}

bool MyMesh::saveFilter(ClientInfo* client) {
  return client->isAdmin();    // only save Admins
}

void MyMesh::loop() {
  mesh::Mesh::loop();

  // Check for network time synchronisation (only when flag is set)
  if (check_netsync_flag) {
    checkNetworkTimeSync();
  }

  if (millisHasNowPassed(next_push) && acl.getNumClients() > 0) {
    // check for ACK timeouts
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->extra.room.pending_ack && millisHasNowPassed(c->extra.room.ack_timeout)) {
        c->extra.room.push_failures++;
        c->extra.room.pending_ack = 0; // reset  (TODO: keep prev expected_ack's in a list, incase they arrive LATER, after we retry)

        // Clear pending system message index to allow retry
        if (pending_system_msg_idx[i] >= 0) {
          MESH_DEBUG_PRINTLN("System message %d ACK timeout for client %02X, will retry", pending_system_msg_idx[i], (uint32_t)c->id.pub_key[0]);
          pending_system_msg_idx[i] = -1;
        }

        MESH_DEBUG_PRINTLN("pending ACK timed out: push_failures: %d", (uint32_t)c->extra.room.push_failures);
      }
    }
    // check next Round-Robin client, and sync next new post
    auto client = acl.getClientByIdx(next_client_idx);
    bool did_push = false;

    // Check for pending system messages first (admin-only)
    // System messages can be sent to non-logged-in admins (up to 3 pre-login attempts)
    if (client->extra.room.pending_ack == 0 && client->isAdmin()) {
      bool is_active = (client->last_activity != 0);

      MESH_DEBUG_PRINTLN("loop - checking for client %02X, isAdmin=%d, is_active=%d, num_sys_msgs=%d",
                         (uint32_t)client->id.pub_key[0], client->isAdmin(), is_active, system_msgs->getNumMessages());

      for (int i = 0; i < system_msgs->getNumMessages(); i++) {
        bool needs_push = system_msgs->needsPush(i, client);

        // Check delivery attempt limit for ALL admins (both active and inactive)
        if (system_msg_prelogin_attempts[next_client_idx][i] >= 3) {
          MESH_DEBUG_PRINTLN("  sys_msg[%d]: skipping, attempts exhausted (%d/3)",
                             i, system_msg_prelogin_attempts[next_client_idx][i]);
          continue;
        }

        MESH_DEBUG_PRINTLN("  sys_msg[%d]: needsPush=%d, attempts=%d",
                           i, needs_push, system_msg_prelogin_attempts[next_client_idx][i]);

        if (needs_push) {
          SystemMessage* sys_msg = system_msgs->getMessage(i);

          // Create temporary PostInfo with timestamp=0 for system message
          PostInfo temp_post;
          temp_post.author = self_id;
          temp_post.post_timestamp = 0;  // Special: system message marker
          StrHelper::strncpy(temp_post.text, sys_msg->text, sizeof(temp_post.text));

          pushPostToClient(client, temp_post);

          // Store which system message is awaiting ACK (don't mark as delivered yet)
          pending_system_msg_idx[next_client_idx] = i;

          // Increment attempt counter for ALL admins (both active and inactive)
          system_msg_prelogin_attempts[next_client_idx][i]++;
          int attempt_num = system_msg_prelogin_attempts[next_client_idx][i];

          if (!is_active) {
            MESH_DEBUG_PRINTLN("loop - pushed system message %d to INACTIVE admin %02X (attempt %d/3), awaiting ACK",
                               i, (uint32_t)client->id.pub_key[0], attempt_num);
          } else {
            MESH_DEBUG_PRINTLN("loop - pushed system message %d to ACTIVE admin %02X (attempt %d/3), awaiting ACK",
                               i, (uint32_t)client->id.pub_key[0], attempt_num);
          }

          // Always-on Serial output for production monitoring
          Serial.printf("SystemMessageQueue: Message %d delivery attempt %d/3 to admin [%02X%02X%02X%02X]\n",
                        i,
                        attempt_num,
                        (uint32_t)client->id.pub_key[0],
                        (uint32_t)client->id.pub_key[1],
                        (uint32_t)client->id.pub_key[2],
                        (uint32_t)client->id.pub_key[3]);

          // If this was the 3rd and final attempt, note it immediately
          if (attempt_num >= 3) {
            Serial.printf("SystemMessageQueue: Message %d attempts exhausted for admin [%02X%02X%02X%02X] - queued until next login\n",
                          i,
                          (uint32_t)client->id.pub_key[0],
                          (uint32_t)client->id.pub_key[1],
                          (uint32_t)client->id.pub_key[2],
                          (uint32_t)client->id.pub_key[3]);
          }

          did_push = true;
          break;  // One message per iteration
        }
      }
    }

    // Push regular posts only if client is active and no system message was pushed
    if (!did_push && client->extra.room.pending_ack == 0 && client->last_activity != 0 &&
        client->extra.room.push_failures < 3) { // not already waiting for ACK, AND not evicted, AND retries not max
      uint32_t now = getRTCClock()->getCurrentTime();
      for (int k = 0, idx = next_post_idx; k < MAX_UNSYNCED_POSTS; k++) {
        auto p = &posts[idx];
        if (now >= p->post_timestamp + POST_SYNC_DELAY_SECS &&
            p->post_timestamp > client->extra.room.sync_since // is new post for this Client?
            && !p->author.matches(client->id)) {   // don't push posts to the author
          // push this post to Client, then wait for ACK
          pushPostToClient(client, *p);
          did_push = true;
          MESH_DEBUG_PRINTLN("loop - pushed to client %02X: %s", (uint32_t)client->id.pub_key[0], p->text);
          break;
        }
        idx = (idx + 1) % MAX_UNSYNCED_POSTS; // wrap to start of cyclic queue
      }
    } else {
      MESH_DEBUG_PRINTLN("loop - skipping busy (or evicted) client %02X", (uint32_t)client->id.pub_key[0]);
    }
    next_client_idx = (next_client_idx + 1) % acl.getNumClients(); // round robin polling for each client

    if (did_push) {
      next_push = futureMillis(SYNC_PUSH_INTERVAL);
    } else {
      // were no unsynced posts for curr client, so proccess next client much quicker! (in next loop())
      next_push = futureMillis(SYNC_PUSH_INTERVAL / 8);
    }
  }

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendFlood(pkt);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_set_params(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_set_params(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs, MyMesh::saveFilter);
    dirty_contacts_expiry = 0;
  }

  // Periodic cleanup of fully-delivered system messages
  static unsigned long next_sys_msg_cleanup = 0;
  if (millisHasNowPassed(next_sys_msg_cleanup)) {
    int old_count = system_msgs->getNumMessages();
    system_msgs->cleanup(&acl);
    int new_count = system_msgs->getNumMessages();
    if (new_count < old_count) {
      system_msgs->save(_fs);  // Save if messages were removed
      MESH_DEBUG_PRINTLN("System message cleanup: removed %d messages", old_count - new_count);
    }
    next_sys_msg_cleanup = futureMillis(60000);  // Cleanup every minute
  }

#ifdef DISPLAY_CLASS
  // Notify UI when new posts are added
  static int last_post_idx_ui = -1;

  if (next_post_idx != last_post_idx_ui) {
    // New post was added - trigger UI notification
    // UI will query getRecentPosts() directly to display
    ui_task.notify(UIEventType::roomMessage);
    last_post_idx_ui = next_post_idx;
  }
#endif

  // TODO: periodically check for OLD/inactive entries in known_clients[], and evict
}

void MyMesh::savePosts() {
  if (!_fs) return;

  // Open file for writing
  File f = openFileForWrite(POSTS_FILE);

  if (!f) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to open posts file for writing");
    return;
  }

  // Write header: version and next_post_idx
  uint8_t version = 1;
  bool success = (f.write(&version, 1) == 1);
  success = success && (f.write((uint8_t*)&next_post_idx, sizeof(next_post_idx)) == sizeof(next_post_idx));

  if (!success) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to write posts header");
    f.close();
    return;
  }

  // Write all posts in the circular buffer (skip system messages with timestamp=0)
  for (int i = 0; i < MAX_UNSYNCED_POSTS; i++) {
    PostInfo* p = &posts[i];

    // Skip system messages (timestamp=0) - don't persist to flash
    if (p->post_timestamp == 0) {
      continue;
    }

    // Write author public key only (Identity only has pub_key)
    success = (f.write(p->author.pub_key, PUB_KEY_SIZE) == PUB_KEY_SIZE);

    // Write timestamp
    success = success && (f.write((uint8_t*)&p->post_timestamp, sizeof(p->post_timestamp)) == sizeof(p->post_timestamp));

    // Write text length and text
    uint8_t text_len = strlen(p->text);
    success = success && (f.write(&text_len, 1) == 1);
    if (text_len > 0) {
      success = success && (f.write((uint8_t*)p->text, text_len) == text_len);
    }

    if (!success) {
      MESH_DEBUG_PRINTLN("ERROR: Failed to write post record");
      break;  // Stop writing on error
    }
  }

  f.close();
  MESH_DEBUG_PRINTLN("Posts saved to flash");
}

void MyMesh::loadPosts() {
  if (!_fs) return;

  if (!_fs->exists(POSTS_FILE)) {
    MESH_DEBUG_PRINTLN("No posts file found - starting fresh");
    return;
  }

  // Open file for reading (platform-specific pattern from companion_radio DataStore)
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File f = _fs->open(POSTS_FILE, FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  File f = _fs->open(POSTS_FILE, "r");
#else
  File f = _fs->open(POSTS_FILE, "r", false);
#endif

  if (!f) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to open posts file for reading");
    return;
  }

  // Read header
  uint8_t version;
  if (f.read(&version, 1) != 1 || version != 1) {
    MESH_DEBUG_PRINTLN("ERROR: Invalid posts file version");
    f.close();
    return;
  }

  if (f.read((uint8_t*)&next_post_idx, sizeof(next_post_idx)) != sizeof(next_post_idx)) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to read next_post_idx");
    f.close();
    return;
  }

  // Read all posts
  for (int i = 0; i < MAX_UNSYNCED_POSTS; i++) {
    PostInfo* p = &posts[i];

    // Read author public key only (Identity only has pub_key)
    if (f.read(p->author.pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE) break;

    // Read timestamp
    if (f.read((uint8_t*)&p->post_timestamp, sizeof(p->post_timestamp)) != sizeof(p->post_timestamp)) break;

    // Read text length and text
    uint8_t text_len;
    if (f.read(&text_len, 1) != 1) break;

    if (text_len > 0 && text_len <= MAX_POST_TEXT_LEN) {
      if (f.read((uint8_t*)p->text, text_len) != text_len) break;
      p->text[text_len] = '\0';
    } else {
      p->text[0] = '\0';
    }
  }

  f.close();
  MESH_DEBUG_PRINTLN("Posts loaded from flash");
}

int MyMesh::getRecentPosts(const PostInfo** dest, int max_posts) const {
  // Return up to max_posts, starting from newest (skip timestamp=0 entries)
  int returned = 0;
  int checked = 0;

  while (returned < max_posts && checked < MAX_UNSYNCED_POSTS) {
    // newest = next_post_idx - 1, next newest = next_post_idx - 2, etc.
    int idx = (next_post_idx - 1 - checked + MAX_UNSYNCED_POSTS) % MAX_UNSYNCED_POSTS;

    if (posts[idx].post_timestamp > 0) {  // Skip system messages
      dest[returned] = &posts[idx];
      returned++;
    }
    checked++;
  }

  return returned;  // Returns 0 if no posts found
}

void MyMesh::notifyUIOfLoadedPosts() {
#ifdef DISPLAY_CLASS
  // Trigger UI refresh to show loaded posts (UI now queries directly via getRecentPosts)
  ui_task.notify(UIEventType::roomMessage);
  MESH_DEBUG_PRINTLN("Triggered UI refresh for loaded posts");
#endif
}
