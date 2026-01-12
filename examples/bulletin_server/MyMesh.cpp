#include "MyMesh.h"
#include "DataStore.h"
#include "SystemMessageHandler.h"
#include "SDStorage.h"
#include "FirmwareCLI.h"
#include "UserCLI.h"

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

// Reply buffer sizes (from uint8_t temp[166])
#define MAX_USER_REPLY_SIZE  157  // temp[9..165] for user commands
#define MAX_CLI_REPLY_SIZE   161  // temp[5..165] for CLI commands

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

  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  posts[next_post_idx].post_timestamp = timestamp;
  next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;

  next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _num_posted++; // stats

  // JSON output for message post
  printJSONSerialLog("post", "create", "message", NULL, postData,
                     client->id.pub_key, "mesh", timestamp);

  // Save posts to flash
  savePosts();
}

void MyMesh::addBulletin(const char* bulletinText, PostSeverity severity) {
  // Length validation - abort if too long (before adding prefix)
  if (strlen(bulletinText) > MAX_POST_TEXT_LEN) {
    return; // Abort - don't post anything
  }

  posts[next_post_idx].author = self_id;  // Use server's identity

  // Add severity prefix
  const char* prefix;
  const char* sev_str;
  switch (severity) {
    case SEVERITY_INFO:
      prefix = SEVERITY_PREFIX_INFO;
      sev_str = "info";
      break;
    case SEVERITY_WARNING:
      prefix = SEVERITY_PREFIX_WARNING;
      sev_str = "warning";
      break;
    case SEVERITY_CRITICAL:
      prefix = SEVERITY_PREFIX_CRITICAL;
      sev_str = "critical";
      break;
    default:
      prefix = SEVERITY_PREFIX_INFO;
      sev_str = "info";
      break;
  }

  // Construct prefixed message
  char prefixed_text[MAX_POST_TEXT_LEN + SEVERITY_PREFIX_LEN + 1];  // +1 for null terminator
  sprintf(prefixed_text, "%s%s", prefix, bulletinText);
  StrHelper::strncpy(posts[next_post_idx].text, prefixed_text, MAX_POST_TEXT_LEN + SEVERITY_PREFIX_LEN);

  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  posts[next_post_idx].post_timestamp = timestamp;
  next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;

  next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _num_posted++; // stats

  // JSON output for bulletin (source is "console" - no user_pubkey)
  printJSONSerialLog("post", "create", "bulletin", sev_str, bulletinText,
                     NULL, "console", timestamp);

  // Broadcast warning and critical bulletins to channel
  if (severity == SEVERITY_WARNING || severity == SEVERITY_CRITICAL) {
    broadcastBulletin(bulletinText, severity);
  }

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
        system_msgs->save(_store->getFS());
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

// Implementations moved to DataStore - these are left for savePrefs and eraseLogFile

void MyMesh::savePrefs() {
  _cli.savePrefs(_store->getFS());
  _store->backupToSD("/com_prefs");
}

void MyMesh::eraseLogFile() {
  _store->removeFile(PACKET_LOG_FILE);
}

static const size_t kCommonPrefsMinSize = 170;
static const size_t kContactsRecordSize = PUB_KEY_SIZE + 1 + 4 + 2 + 1 + MAX_PATH_SIZE + PUB_KEY_SIZE;

bool MyMesh::isFlashConfigUsable(const char* filename, size_t min_size, size_t size_alignment) const {
  if (!_store || !_store->exists(filename)) {
    return false;
  }

  File file = _store->openRead(filename);
  if (!file) {
    return false;
  }

  size_t file_size = file.size();
  if (min_size > 0 && file_size < min_size) {
    file.close();
    return false;
  }

  if (size_alignment > 0 && (file_size % size_alignment) != 0) {
    file.close();
    return false;
  }

  file.close();
  return true;
}

void MyMesh::backupConfigToSD() {
  if (!_store) return;
  _store->backupToSD("/com_prefs");
  _store->backupToSD("/s_contacts");
  _store->backupToSD("/channel_cfg");
  _store->backupToSD("/netsync_cfg");
}

void MyMesh::restoreConfigFromSDIfNeeded() {
  if (!_store) return;
  if (!isFlashConfigUsable("/com_prefs", kCommonPrefsMinSize)) {
    _store->restoreFromSD("/com_prefs");
  }
  if (!isFlashConfigUsable("/s_contacts", 0, kContactsRecordSize)) {
    _store->restoreFromSD("/s_contacts");
  }
  if (!isFlashConfigUsable("/channel_cfg", sizeof(BulletinChannelConfig))) {
    _store->restoreFromSD("/channel_cfg");
  }
  if (!isFlashConfigUsable("/netsync_cfg", sizeof(ClockNetSyncConfig))) {
    _store->restoreFromSD("/netsync_cfg");
  }
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload,
                          size_t payload_len) {
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
    File f = _store->openAppend(PACKET_LOG_FILE);
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
    File f = _store->openAppend(PACKET_LOG_FILE);
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
    File f = _store->openAppend(PACKET_LOG_FILE);
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
    uint8_t perm = 0;
    if (data[8] == 0 && !_prefs.allow_read_only) {   // blank password, just check if sender is in ACL
      client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
      if (client == NULL) {
      #if MESH_DEBUG
        MESH_DEBUG_PRINTLN("Login, sender not in ACL");
      #endif
      } else {
        perm = client->permissions;
      }
    }
    if (client == NULL) {
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
    }

    // Clock synchronisation from admin login
    MESH_DEBUG_PRINTLN("Login: perm=%d, isDesynced=%d, clock_synced_once=%d, sender_ts=%lu",
                       perm, isDesynced(), clock_synced_once, sender_timestamp);
    if ((perm & PERM_ACL_ROLE_MASK) == PERM_ACL_ADMIN && isDesynced() && !clock_synced_once) {
      if (sender_timestamp >= MIN_VALID_TIMESTAMP) {
        getRTCClock()->setCurrentTime(sender_timestamp);
        notifyClockSynced(sender.pub_key);

        // Schedule immediate post sync check to push any pending posts
        next_push = 0;

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

    // Track login in history
    trackLogin(client->id.pub_key, perm, client->last_activity);

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

bool MyMesh::handleUserCommand(ClientInfo* client, mesh::Packet* packet, const char* command, char* reply) {
  return _user_cli->handleCommand(client, packet, command, reply);
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

        // Schedule immediate post sync check to push any pending posts
        next_push = 0;

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
            handleCommand(sender_timestamp, (char *)&data[5], (char *)&temp[5], client);
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
          // Check if this is a user command (starts with '!')
          const char* message = (const char*)&data[5];
          if (message[0] == '!') {
            // Handle user command
            if (!is_retry) {
              handleUserCommand(client, packet, message, (char*)&temp[9]);
              temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2); // Send reply as TXT_TYPE_SIGNED_PLAIN with server identity
              // Encode prefix of server's pub_key (matching system message format)
              memcpy(&temp[5], self_id.pub_key, 4);
            } else {
              temp[9] = 0; // no reply on retry
            }
            send_ack = true; // Send ACK so client knows command was received
          } else {
            // Block posting if clock is desynced
            if (isDesynced()) {
              strcpy((char*)&temp[9], "Error: Server clock desynced");
              temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2); // Send error as TXT_TYPE_SIGNED_PLAIN with server identity
              // Encode prefix of server's pub_key (matching system message format)
              memcpy(&temp[5], self_id.pub_key, 4);
              send_ack = false; // No ACK for error
            } else {
              if (!is_retry) {
                addPost(client, (const char *)&data[5]);
              }
              temp[9] = 0; // no reply (ACK is enough)
              send_ack = true;
            }
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

      int text_len = strlen((char *)&temp[9]);
      if (text_len > 0) {
        if (now == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          now++;
        }
        memcpy(temp, &now, 4); // mostly an extra blob to help make packet_hash unique

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 9 + text_len);
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

void MyMesh::trackLogin(const uint8_t* pub_key, uint8_t permissions, uint32_t timestamp) {
  // Add to circular buffer
  auto& entry = login_history[login_history_next_idx];
  memcpy(entry.pub_key, pub_key, 4);
  entry.timestamp = timestamp;
  entry.permissions = permissions;

  // Update circular buffer indices
  login_history_next_idx = (login_history_next_idx + 1) % 5;
  if (login_history_count < 5) {
    login_history_count++;
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cli(board, rtc, sensors, &_prefs, this), telemetry(MAX_PACKET_PAYLOAD - 4) {
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  _logging = false;
  _store = nullptr;
  set_radio_at = revert_radio_at = 0;
  current_boot_sequence = 0;
  system_msgs = new SystemMessageHandler();
  _firmware_cli = new FirmwareCLI(this);
  _user_cli = new UserCLI(this);
  clock_synced_once = false;

  // Network time sync initialisation
  repeater_count = 0;
  check_netsync_flag = false;
  memset(&netsync_config, 0, sizeof(netsync_config));

  // Bulletin rate limiting initialisation
  last_bulletin_time = 0;
  memset(repeater_buffer, 0, sizeof(repeater_buffer));
  netsync_config.enabled = 0;         // Default: disabled (admin sync preferred)
  netsync_config.maxwait_mins = 15;   // Default: 15 minutes
  netsync_config.guard = 0xDEADBEEF;  // Validation marker

  // Login history initialisation
  memset(login_history, 0, sizeof(login_history));
  login_history_count = 0;
  login_history_next_idx = 0;
  memset(pending_app_request_times, 0, sizeof(pending_app_request_times));

  // Broadcast channel initialisation
  memset(&channel_config, 0, sizeof(channel_config));
  memset(&bulletin_channel, 0, sizeof(bulletin_channel));
  channel_initialised = false;

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;   // one half
  _prefs.rx_delay_base = 0.0f;   // off by default, was 10.0
  _prefs.tx_delay_factor = 0.5f; // was 0.25f;
  _prefs.direct_tx_delay_factor = 0.2f; // v1.11.0
  _prefs.gps_enabled = 0;  // v1.11.0
  _prefs.gps_interval = 0; // v1.11.0
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS; // v1.11.0
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
#else
  StrHelper::strncpy(_prefs.guest_password, "hello", sizeof(_prefs.guest_password));
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
  File file = _store->openWrite("/boot_count");

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
  system_msgs->save(_store->getFS());
  // Note: Serial output now handled by SystemMessageQueue::addMessage()
  MESH_DEBUG_PRINTLN("Added system message (boot %u), now have %d messages",
                     current_boot_sequence, system_msgs->getNumMessages());
}

// ----------------------- Network Time Synchronisation -----------------------

void MyMesh::loadNetSyncConfig() {
  ClockNetSyncConfig loaded_config;
  bool loaded = false;

  File f = _store->openRead( "/netsync_cfg");
  if (f) {
    size_t bytes_read = f.read((uint8_t*)&loaded_config, sizeof(loaded_config));
    f.close();

    if (bytes_read == sizeof(loaded_config) && loaded_config.guard == 0xDEADBEEF) {
      if (loaded_config.maxwait_mins >= 5 && loaded_config.maxwait_mins <= 60) {
        loaded = true;
      } else {
        MESH_DEBUG_PRINTLN("Invalid maxwait_mins in config, using defaults");
      }
    }
  }

  if (!loaded && _store->restoreFromSD("/netsync_cfg")) {
    File rf = _store->openRead( "/netsync_cfg");
    if (rf) {
      size_t bytes_read = rf.read((uint8_t*)&loaded_config, sizeof(loaded_config));
      rf.close();
      if (bytes_read == sizeof(loaded_config) && loaded_config.guard == 0xDEADBEEF &&
          loaded_config.maxwait_mins >= 5 && loaded_config.maxwait_mins <= 60) {
        loaded = true;
      }
    }
  }

  if (loaded) {
    netsync_config = loaded_config;
    MESH_DEBUG_PRINTLN("Loaded network time sync config: enabled=%d, maxwait=%d min",
                      netsync_config.enabled, netsync_config.maxwait_mins);
  } else {
    MESH_DEBUG_PRINTLN("Invalid or missing network time sync config, using defaults");
  }
}

void MyMesh::saveNetSyncConfig() {
  File f = _store->openWrite( "/netsync_cfg");
  if (f) {
    f.write((uint8_t*)&netsync_config, sizeof(netsync_config));
    f.close();
    _store->backupToSD("/netsync_cfg");
    MESH_DEBUG_PRINTLN("Saved network time sync config");
  }
}

void MyMesh::loadChannelConfig() {
  BulletinChannelConfig loaded_config;
  bool loaded = false;

  File f = _store->openRead( CHANNEL_CONFIG_FILE);
  if (f) {
    size_t bytes_read = f.read((uint8_t*)&loaded_config, sizeof(loaded_config));
    f.close();

    if (bytes_read == sizeof(loaded_config) && loaded_config.guard == 0xDEADBEEF) {
      loaded = true;
    }
  }

  if (!loaded && _store->restoreFromSD(CHANNEL_CONFIG_FILE)) {
    File rf = _store->openRead( CHANNEL_CONFIG_FILE);
    if (rf) {
      size_t bytes_read = rf.read((uint8_t*)&loaded_config, sizeof(loaded_config));
      rf.close();
      if (bytes_read == sizeof(loaded_config) && loaded_config.guard == 0xDEADBEEF) {
        loaded = true;
      }
    }
  }

  if (loaded) {
    channel_config = loaded_config;
    MESH_DEBUG_PRINTLN("Loaded channel config: mode_private=%d", channel_config.mode_private);
  } else {
    MESH_DEBUG_PRINTLN("Invalid or missing channel config, using defaults");
    channel_config.mode_private = false;  // Public mode by default
    memset(channel_config.secret, 0, sizeof(channel_config.secret));
    channel_config.guard = 0xDEADBEEF;
    saveChannelConfig();  // Save defaults
  }
}

void MyMesh::saveChannelConfig() {
  File f = _store->openWrite( CHANNEL_CONFIG_FILE);
  if (f) {
    f.write((uint8_t*)&channel_config, sizeof(channel_config));
    f.close();
    _store->backupToSD(CHANNEL_CONFIG_FILE);
    MESH_DEBUG_PRINTLN("Saved channel config");
  }
}

void MyMesh::initialiseChannel() {
  // Clear the runtime secret buffer
  memset(bulletin_channel.secret, 0, sizeof(bulletin_channel.secret));
  if (channel_config.mode_private) {
    // Private mode: use stored secret
    memcpy(bulletin_channel.secret, channel_config.secret, CHANNEL_KEY_LEN);
  } else {
    // Public mode: Derive secret from server's public key (first n bytes as defined in CHANNEL_KEY_LEN of self_id.pub_key)
    memcpy(bulletin_channel.secret, self_id.pub_key, CHANNEL_KEY_LEN);
  }

  // Compute hash of secret (first byte only, as per GroupChannel structure)
  uint8_t full_hash[32];
  mesh::Utils::sha256(full_hash, 32, bulletin_channel.secret, CHANNEL_KEY_LEN);
  bulletin_channel.hash[0] = full_hash[0];

  channel_initialised = true;
  MESH_DEBUG_PRINTLN("Initialised channel: mode=%s, hash[0]=0x%02X",
                     channel_config.mode_private ? "private" : "public",
                     bulletin_channel.hash[0]);
}

void MyMesh::setChannelModePublic() {
  if (!channel_config.mode_private) {
    return;  // Already in public mode
  }

  channel_config.mode_private = false;
  memset(channel_config.secret, 0, sizeof(channel_config.secret));
  saveChannelConfig();
  initialiseChannel();

  // Add system message
  addSystemMessage("Channel mode changed to public");

  // JSON output
  printJSONSerialLog("channel", "config", "mode", NULL, "public",
                     NULL, "console", getRTCClock()->getCurrentTime());
}

void MyMesh::setChannelModePrivate() {
  if (channel_config.mode_private) {
    return;  // Already in private mode
  }

  // Generate new random channel secret
  getRNG()->random(channel_config.secret, CHANNEL_KEY_LEN);
  channel_config.mode_private = true;
  saveChannelConfig();
  initialiseChannel();

  // Add system message
  addSystemMessage("Channel mode changed to private");

  // JSON output - include secret in hex for admin to share
  Serial.print("{\"component\":\"channel\",\"action\":\"config\",\"data\":{\"type\":\"mode\",\"mode\":\"private\",\"secret\":\"");
  mesh::Utils::printHex(Serial, channel_config.secret, CHANNEL_KEY_LEN);
  Serial.print("\"},\"meta\":{\"source\":\"console\",\"timestamp\":");
  Serial.print(getRTCClock()->getCurrentTime());
  Serial.println("}}");
}

void MyMesh::broadcastBulletin(const char* bulletinText, PostSeverity severity) {
  if (!channel_initialised) {
    MESH_DEBUG_PRINTLN("Cannot broadcast - channel not initialised");
    return;
  }

  // Length check (bulletin text only, without prefix)
  if (strlen(bulletinText) > MAX_POST_TEXT_LEN) {
    MESH_DEBUG_PRINTLN("Bulletin too long to broadcast");
    return;
  }

  // Determine severity prefix and string
  const char* prefix;
  const char* sev_str;
  switch (severity) {
    case SEVERITY_WARNING:
      prefix = SEVERITY_PREFIX_WARNING;
      sev_str = "warning";
      break;
    case SEVERITY_CRITICAL:
      prefix = SEVERITY_PREFIX_CRITICAL;
      sev_str = "critical";
      break;
    default:
      // Should never happen, but default to critical
      prefix = SEVERITY_PREFIX_CRITICAL;
      sev_str = "critical";
      break;
  }

  // Build the prefixed message
  char prefixed_text[MAX_POST_TEXT_LEN + SEVERITY_PREFIX_LEN + 1];
  sprintf(prefixed_text, "%s%s", prefix, bulletinText);

  // Format payload with sender name prefix (matching companion_radio format)
  uint8_t payload[MAX_PACKET_PAYLOAD];
  int i = 0;

  uint32_t timestamp = getRTCClock()->getCurrentTime();
  memcpy(&payload[i], &timestamp, 4);  // [0-3]: timestamp
  i += 4;

  payload[i++] = 0;  // [4]: TXT_TYPE_PLAIN (channels only support PLAIN)

  // [5+]: "<sender_name>: <prefixed_text>"
  int name_len = strlen(_prefs.node_name);
  if (i + name_len + 2 < MAX_PACKET_PAYLOAD) {  // +2 for ": "
    memcpy(&payload[i], _prefs.node_name, name_len);
    i += name_len;
    payload[i++] = ':';
    payload[i++] = ' ';
  }

  int text_len = strlen(prefixed_text);
  if (i + text_len + 1 > MAX_PACKET_PAYLOAD) {
    MESH_DEBUG_PRINTLN("broadcastBulletin: message too long (%d bytes), truncating", text_len);
    text_len = MAX_PACKET_PAYLOAD - i - 1;  // Leave room for null terminator
  }
  memcpy(&payload[i], prefixed_text, text_len + 1);  // +1 for null terminator
  i += text_len + 1;

  // Send as group message on the bulletin channel
  mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, bulletin_channel, payload, i);
  if (pkt) {
    sendFlood(pkt);
  }

  MESH_DEBUG_PRINTLN("Broadcast %s bulletin to channel", sev_str);

  // JSON output
  printJSONSerialLog("channel", "broadcast", "bulletin", sev_str, bulletinText,
                     NULL, "console", timestamp);
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

  // Schedule immediate post sync check to push any pending posts
  next_push = 0;

  // Clear buffer
  repeater_count = 0;
}

void MyMesh::notifyClockSyncedFromRepeaters() {
  char msg[MAX_POST_TEXT_LEN + 1];

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

  int len = snprintf(msg, sizeof(msg), "Clock set by Repeater advert from [%02X%02X%02X%02X] to %02d %s %04d %02d:%02d. Quorum nodes: ",
          repeater_buffer[most_recent_idx].pub_key[0],
          repeater_buffer[most_recent_idx].pub_key[1],
          repeater_buffer[most_recent_idx].pub_key[2],
          repeater_buffer[most_recent_idx].pub_key[3],
          dt.day(),
          months[dt.month() - 1],
          dt.year(),
          dt.hour(),
          dt.minute());

  // Add all repeater IDs to quorum list with bounds checking
  for (int i = 0; i < repeater_count && i < 3; i++) {
    if (len >= (int)sizeof(msg) - 15) break;  // Reserve space for "[AABBCCDD], " (14 chars) + null
    len += snprintf(msg + len, sizeof(msg) - len, "[%02X%02X%02X%02X]",
            repeater_buffer[i].pub_key[0],
            repeater_buffer[i].pub_key[1],
            repeater_buffer[i].pub_key[2],
            repeater_buffer[i].pub_key[3]);
    if (i < repeater_count - 1 && len < (int)sizeof(msg) - 3) {
      len += snprintf(msg + len, sizeof(msg) - len, ", ");
    }
  }
  if (len < (int)sizeof(msg) - 1) {
    snprintf(msg + len, sizeof(msg) - len, ".");
  }

  addSystemMessage(msg);
}

// ----------------------------------------------------------------------------

void MyMesh::begin(DataStore* store) {
  mesh::Mesh::begin();
  _store = store;

  restoreConfigFromSDIfNeeded();

  FILESYSTEM* fs = _store->getFS();

  // Load and increment boot counter
  current_boot_sequence = loadBootCounter(fs);
  current_boot_sequence++;
  saveBootCounter(fs, current_boot_sequence);

  // load persisted prefs
  _cli.loadPrefs(fs);

  acl.load(fs);

  // Load persisted posts
  loadPosts();

  // Load system message queue
  system_msgs->load(fs);
  MESH_DEBUG_PRINTLN("Loaded %d system messages from flash", system_msgs->getNumMessages());

  // Load network time sync configuration
  loadNetSyncConfig();

  // Load broadcast channel configuration
  loadChannelConfig();
  initialiseChannel();

  backupConfigToSD();

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

  board.setAdcMultiplier(_prefs.adc_multiplier);  // v1.11.0

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();  // v1.11.0
#endif
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
  File f = _store->openRead(PACKET_LOG_FILE);
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
  _store->saveMainIdentity(self_id);
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(),
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

// JSON serial output helper - outputs structured log data
void MyMesh::printJSONSerialLog(const char* component, const char* action, const char* type,
                                 const char* severity, const char* text,
                                 const uint8_t* user_pubkey, const char* source,
                                 uint32_t timestamp) {
  Serial.print("{\"component\":\"");
  Serial.print(component);
  Serial.print("\",\"action\":\"");
  Serial.print(action);

  // Data section
  Serial.print("\",\"data\":{\"type\":\"");
  Serial.print(type);
  Serial.print("\"");

  if (severity) {
    Serial.print(",\"severity\":\"");
    Serial.print(severity);
    Serial.print("\"");
  }

  if (text) {
    Serial.print(",\"text\":\"");
    // TODO: Escape quotes in text for proper JSON
    Serial.print(text);
    Serial.print("\"");
  }

  Serial.print("}");

  // Meta section
  Serial.print(",\"meta\":{");

  if (user_pubkey) {
    Serial.print("\"user_pubkey\":\"");
    mesh::Utils::printHex(Serial, user_pubkey, PUB_KEY_SIZE);
    Serial.print("\"");

    // Look up user role from ACL
    ClientInfo* client = acl.getClient(user_pubkey, PUB_KEY_SIZE);
    if (client) {
      uint8_t role = client->permissions & PERM_ACL_ROLE_MASK;
      Serial.print(",\"user_role\":\"");
      switch (role) {
        case PERM_ACL_ADMIN:      Serial.print("admin"); break;
        case PERM_ACL_READ_WRITE: Serial.print("read_write"); break;
        case PERM_ACL_READ_ONLY:  Serial.print("read_only"); break;
        case PERM_ACL_GUEST:
        default:                  Serial.print("guest"); break;
      }
      Serial.print("\"");
    } else {
      // Not in ACL - must be guest
      Serial.print(",\"user_role\":\"guest\"");
    }
    Serial.print(",");
  }

  // Output source (auto-detect if NULL: mesh if user_pubkey, console otherwise)
  Serial.print("\"source\":\"");
  if (source) {
    Serial.print(source);
  } else {
    Serial.print(user_pubkey ? "mesh" : "console");
  }
  Serial.print("\"");

  Serial.print(",\"timestamp\":");
  Serial.print(timestamp);
  Serial.println("}}");
}

// FirmwareCLI helper methods
void MyMesh::scheduleLazyWrite() {
  dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
}

bool MyMesh::checkBulletinRateLimit(char* reply) {
  if (last_bulletin_time > 0 && millis() - last_bulletin_time < BULLETIN_RATE_LIMIT_MILLIS) {
    uint32_t remaining = (BULLETIN_RATE_LIMIT_MILLIS / 1000) - ((millis() - last_bulletin_time) / 1000);
    sprintf(reply, "ERROR: Rate limit hit. Wait %u seconds.", remaining);
    return false;
  }
  return true;
}

void MyMesh::updateBulletinRateLimit() {
  last_bulletin_time = millis();
}

void MyMesh::setNetsyncEnabled(bool enabled) {
  netsync_config.enabled = enabled ? 1 : 0;
  saveNetSyncConfig();
}

bool MyMesh::isNetsyncEnabled() const {
  return netsync_config.enabled != 0;
}

void MyMesh::setNetsyncMaxwait(int mins) {
  netsync_config.maxwait_mins = mins;
  saveNetSyncConfig();
}

int MyMesh::getNetsyncMaxwait() const {
  return netsync_config.maxwait_mins;
}

bool MyMesh::isClockSynced() const {
  return clock_synced_once;
}

int MyMesh::getRepeaterCount() const {
  return repeater_count;
}

bool MyMesh::isChannelPrivate() const {
  return channel_config.mode_private;
}

int MyMesh::getLoginHistoryCount() const {
  return login_history_count;
}

bool MyMesh::getLoginHistoryEntry(int idx, LoginHistoryEntry& entry) const {
  if (idx < 0 || idx >= login_history_count) return false;
  // Iterate backwards through circular buffer (newest first)
  int buf_idx = (login_history_next_idx - 1 - idx + 5) % 5;
  entry = login_history[buf_idx];
  return true;
}

bool MyMesh::sendAppReply(const char* app_name, const uint8_t* pubkey, const char* response_text) {
  // Find the client with this pubkey
  ClientInfo* target_client = NULL;
  int target_client_idx = -1;
  for (int i = 0; i < acl.getNumClients(); i++) {
    auto c = acl.getClientByIdx(i);
    if (memcmp(c->id.pub_key, pubkey, PUB_KEY_SIZE) == 0) {
      target_client = c;
      target_client_idx = i;
      break;
    }
  }

  if (!target_client) return false;

  // Clear pending app request flag
  pending_app_request_times[target_client_idx] = 0;

  // Send response to user (matching user command reply format)
  uint8_t temp[166];
  uint32_t now = getRTCClock()->getCurrentTime();
  memcpy(temp, &now, 4);  // timestamp
  temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2);  // Send as signed message
  memcpy(&temp[5], self_id.pub_key, 4);    // Server identity prefix
  strcpy((char*)&temp[9], response_text);

  // Send reply
  mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, target_client->id,
                                     target_client->shared_secret,
                                     temp, 9 + strlen((char*)&temp[9]) + 1);
  if (pkt) {
    if (target_client->out_path_len > 0) {
      sendDirect(pkt, target_client->out_path, target_client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFlood(pkt, SERVER_RESPONSE_DELAY);
    }
  }

  // Log to JSON (include app_name in the log)
  Serial.print("{\"component\":\"app\",\"action\":\"response\",\"data\":{\"type\":\"data\",\"app_name\":\"");
  Serial.print(app_name);
  Serial.print("\",\"text\":\"");
  Serial.print(response_text);
  Serial.print("\"},\"meta\":{\"user_pubkey\":\"");
  mesh::Utils::printHex(Serial, target_client->id.pub_key, PUB_KEY_SIZE);
  Serial.print("\",\"source\":\"console\",\"timestamp\":");
  Serial.print(getRTCClock()->getCurrentTime());
  Serial.println("}}");

  return true;
}

// UserCLI helper methods
void MyMesh::logUserCommand(const char* action, const char* text, const uint8_t* user_pubkey, uint32_t timestamp) {
  printJSONSerialLog("app", action, "command", NULL, text, user_pubkey, "mesh", timestamp);
}

void MyMesh::formatChannelKey(char* dest, size_t len) {
  char* pos = dest;
  size_t remaining = len;

  if (channel_config.mode_private) {
    // Channel mode is private - return secret key from config
    for (int i = 0; i < CHANNEL_KEY_LEN && remaining > 3; i++) {
      snprintf(pos, remaining, "%02X", channel_config.secret[i]);
      pos += 2;
      remaining -= 2;
    }
  } else {
    // Channel mode is public - print node public key
    for (int i = 0; i < CHANNEL_KEY_LEN && remaining > 3; i++) {
      snprintf(pos, remaining, "%02X", self_id.pub_key[i]);
      pos += 2;
      remaining -= 2;
    }
  }
  *pos = 0;
}

int MyMesh::getClientIndex(const ClientInfo* client) const {
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i) == client) {
      return i;
    }
  }
  return -1;
}

void MyMesh::markPendingAppRequest(ClientInfo* client) {
  int client_idx = getClientIndex(client);
  if (client_idx >= 0 && client_idx < MAX_CLIENTS) {
    pending_app_request_times[client_idx] = millis();
  }
}

bool MyMesh::hasPendingAppRequest(const ClientInfo* client) const {
  int client_idx = getClientIndex(client);
  if (client_idx < 0 || client_idx >= MAX_CLIENTS) {
    return false;
  }
  return pending_app_request_times[client_idx] != 0;
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply, ClientInfo* client) {
  while (*command == ' ')
    command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // Try firmware-specific CLI commands first
  if (_firmware_cli->handleCommand(sender_timestamp, command, reply)) {
    return;
  }

  // Fall through to common CLI commands
  bool was_desynced = isDesynced();
  _cli.handleCommand(sender_timestamp, command, reply);

  // If clock was desynced and now isn't, notify clock sync
  if (was_desynced && !isDesynced()) {
    notifyClockSynced(NULL);  // NULL = manual sync via CLI
    // Schedule immediate post sync check to push any pending posts
    next_push = 0;
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

      // Check for app request timeouts (10 seconds)
      if (pending_app_request_times[i] != 0) {
        unsigned long elapsed = millis() - pending_app_request_times[i];
        if (elapsed >= 10000) {  // 10 second timeout
          // Clear the pending flag
          pending_app_request_times[i] = 0;

          // Send timeout message to user (matching user command reply format)
          uint8_t temp[166];
          uint32_t now = getRTCClock()->getCurrentTime();
          memcpy(temp, &now, 4);  // timestamp
          temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2);  // Send as signed message
          memcpy(&temp[5], self_id.pub_key, 4);    // Server identity prefix
          strcpy((char*)&temp[9], "Request timeout - no response from app");

          mesh::Packet* pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id,
                                             c->shared_secret,
                                             temp, 9 + strlen((char*)&temp[9]) + 1);
          if (pkt) {
            if (c->out_path_len > 0) {
              sendDirect(pkt, c->out_path, c->out_path_len, SERVER_RESPONSE_DELAY);
            } else {
              sendFlood(pkt, SERVER_RESPONSE_DELAY);
            }
          }

          MESH_DEBUG_PRINTLN("App request timeout for client %02X", (uint32_t)c->id.pub_key[0]);
        }
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
    acl.save(_store->getFS(), MyMesh::saveFilter);
    _store->backupToSD("/s_contacts");
    dirty_contacts_expiry = 0;
  }

  // Periodic cleanup of fully-delivered system messages
  static unsigned long next_sys_msg_cleanup = 0;
  if (millisHasNowPassed(next_sys_msg_cleanup)) {
    int old_count = system_msgs->getNumMessages();
    system_msgs->cleanup(&acl);
    int new_count = system_msgs->getNumMessages();
    if (new_count < old_count) {
      system_msgs->save(_store->getFS());  // Save if messages were removed
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
#if SD_SUPPORTED
  if (!_store) return;
  SDStorage* sd = _store->getSD();
  if (!sd || !sd->isReady()) {
    MESH_DEBUG_PRINTLN("SD card not available - posts not persisted");
    return;
  }

  // Open file for writing on SD card
  File f = sd->openForWrite(POSTS_FILE);

  if (!f) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to open posts file on SD for writing");
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

    // Skip system messages (timestamp=0) - don't persist to SD
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
      MESH_DEBUG_PRINTLN("ERROR: Failed to write post record to SD");
      break;  // Stop writing on error
    }
  }

  f.close();
  MESH_DEBUG_PRINTLN("Posts saved to SD card");
#else
  // SD not supported on this platform - posts only exist in RAM
  MESH_DEBUG_PRINTLN("SD not supported - posts not persisted");
#endif
}

void MyMesh::loadPosts() {
#if SD_SUPPORTED
  if (!_store) return;
  SDStorage* sd = _store->getSD();
  if (!sd || !sd->isReady()) {
    MESH_DEBUG_PRINTLN("SD card not available - no posts loaded");
    return;
  }

  if (!sd->exists(POSTS_FILE)) {
    MESH_DEBUG_PRINTLN("No posts file on SD - starting fresh");
    return;
  }

  File f = sd->openForRead(POSTS_FILE);

  if (!f) {
    MESH_DEBUG_PRINTLN("ERROR: Failed to open posts file on SD for reading");
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

    size_t max_text_len = sizeof(p->text) - 1;
    bool read_ok = true;
    if (text_len > 0) {
      if (text_len <= max_text_len) {
        read_ok = (f.read((uint8_t*)p->text, text_len) == text_len);
        if (read_ok) {
          p->text[text_len] = '\0';
        }
      } else {
        size_t remaining = text_len;
        uint8_t discard[32];
        while (remaining > 0) {
          size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
          int bytes_read = f.read(discard, chunk);
          if (bytes_read != (int)chunk) {
            read_ok = false;
            break;
          }
          remaining -= chunk;
        }
        p->text[0] = '\0';
      }
    } else {
      p->text[0] = '\0';
    }
    if (!read_ok) break;
  }

  f.close();
  MESH_DEBUG_PRINTLN("Posts loaded from SD card");
#else
  // SD not supported on this platform - no posts to load
  MESH_DEBUG_PRINTLN("SD not supported - no posts loaded");
#endif
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
