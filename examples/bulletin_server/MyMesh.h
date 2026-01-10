#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/CommonCLI.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/ClientACL.h>
#include <RTClib.h>
#include <target.h>

// Forward declarations
class SystemMessageQueue;
class SDStorage;

/* ------------------------------ Config -------------------------------- */

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "10 Jan 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.0.0"
#endif

#define MESHCORE_VERSION "1.11.0"

#ifndef LORA_FREQ
  #define LORA_FREQ   915.0
#endif
#ifndef LORA_BW
  #define LORA_BW     250
#endif
#ifndef LORA_SF
  #define LORA_SF     10
#endif
#ifndef LORA_CR
  #define LORA_CR      5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  20
#endif

#ifndef ADVERT_NAME
  #define  ADVERT_NAME   "Bulletin Server"
#endif
#ifndef ADVERT_LAT
  #define  ADVERT_LAT  0.0
#endif
#ifndef ADVERT_LON
  #define  ADVERT_LON  0.0
#endif

#ifndef ADMIN_PASSWORD
  #define  ADMIN_PASSWORD  "password"
#endif

#ifndef MAX_UNSYNCED_POSTS
  #define MAX_UNSYNCED_POSTS    32
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY   300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY     200
#endif

#define BULLETIN_RATE_LIMIT_MILLIS  10000  // 10 seconds between bulletin posts

#define FIRMWARE_ROLE "room_server"

#define PACKET_LOG_FILE     "/packet_log"
#define POSTS_FILE          "/posts"
#define CHANNEL_CONFIG_FILE "/channel_cfg"

#define MAX_POST_TEXT_LEN    140  // User message limit (prefix added on top)
#define CHANNEL_KEY_LEN 16 // Channel key byte length (only used for private mode channels)
// Bulletin severity prefixes
#define SEVERITY_PREFIX_INFO     "BLTN-INFO: "
#define SEVERITY_PREFIX_WARNING  "BLTN-WARN: "
#define SEVERITY_PREFIX_CRITICAL "BLTN-CRIT: "
#define SEVERITY_PREFIX_LEN      11  // Length of severity prefix (all are 11 chars)

struct PostInfo {
  mesh::Identity author;
  uint32_t post_timestamp;   // by OUR clock
  char text[MAX_POST_TEXT_LEN+12];  // +12 for "BLTN-CRIT: " prefix (11 chars + null)
};

// Bulletin severity levels
enum PostSeverity {
  SEVERITY_INFO = 0,
  SEVERITY_WARNING = 1,
  SEVERITY_CRITICAL = 2
};

// Login history entry (runtime only)
struct LoginHistoryEntry {
  uint8_t pub_key[4];        // First 4 bytes of user's public key
  uint32_t timestamp;        // Login timestamp (by our clock)
  uint8_t permissions;       // User permissions at login
};

// Broadcast channel configuration (persistent)
struct BulletinChannelConfig {
  bool mode_private;         // false=public, true=private (default: false)
  uint8_t secret[CHANNEL_KEY_LEN];        // Private channel key (only used if mode_private==true)
  uint32_t guard;            // 0xDEADBEEF validation marker
};

// Network time synchronisation configuration (persistent)
struct ClockNetSyncConfig {
  uint8_t enabled;           // 0=off, 1=on (default: 0)
  uint16_t maxwait_mins;     // Max agreement window in minutes (default: 15, range: 5-60)
  uint32_t guard;            // 0xDEADBEEF validation marker
};

// Repeater advertisement buffer entry (runtime only)
struct RepeaterAdvert {
  uint8_t pub_key[4];        // First 4 bytes of repeater's public key for identification
  uint32_t timestamp;        // Unix timestamp from repeater's advert
  uint32_t received_time;    // Our clock time when advert was received (for aging)
};

// Bulletin server - manages posts, client sync, and flash persistence
class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  SDStorage* _sd;  // SD card storage (ESP32 only, may be null)
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  CommonCLI _cli;
  ClientACL acl;
  unsigned long dirty_contacts_expiry;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  unsigned long next_push;
  uint16_t _num_posted, _num_post_pushes;
  int next_client_idx;  // for round-robin polling
  int next_post_idx;
  PostInfo posts[MAX_UNSYNCED_POSTS];   // cyclic queue
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
  uint32_t current_boot_sequence;
  SystemMessageQueue* system_msgs;
  bool clock_synced_once;  // Track if clock has been synced this boot
  int16_t pending_system_msg_idx[MAX_CLIENTS];  // System message index awaiting ACK per client (-1 = none)
  uint8_t system_msg_prelogin_attempts[MAX_CLIENTS][8];  // Pre-login delivery attempts per client per message (max 8 system messages)

  // Network time synchronisation state
  ClockNetSyncConfig netsync_config;  // Persistent configuration
  RepeaterAdvert repeater_buffer[3];  // Buffer for up to 3 repeater adverts
  uint8_t repeater_count;             // Number of valid adverts in buffer
  bool check_netsync_flag;            // Flag to trigger sync check on next loop

  // Bulletin rate limiting
  uint32_t last_bulletin_time;        // Last bulletin post time for rate limiting

  // External app request tracking
  unsigned long pending_app_request_times[MAX_CLIENTS];  // millis() when !app request was sent (0 = none pending)

  // Login history tracking (last 5 logins, circular buffer)
  LoginHistoryEntry login_history[5];
  uint8_t login_history_count;       // Number of entries (0-5)
  uint8_t login_history_next_idx;    // Next write position

  // Broadcast channel state
  BulletinChannelConfig channel_config;  // Persistent configuration
  mesh::GroupChannel bulletin_channel;   // Runtime channel (hash + secret)
  bool channel_initialised;              // True once channel is loaded/initialised

  void addPost(ClientInfo* client, const char* postData);
  void pushPostToClient(ClientInfo* client, PostInfo& post);
  uint8_t getUnsyncedCount(ClientInfo* client);
  bool handleUserCommand(ClientInfo* client, mesh::Packet* packet, const char* command, char* reply);
  void trackLogin(const uint8_t* pub_key, uint8_t permissions, uint32_t timestamp);
  bool processAck(const uint8_t *data);
  mesh::Packet* createSelfAdvert();
  File openAppend(const char* fname);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  void savePosts();
  void loadPosts();
  uint32_t loadBootCounter(FILESYSTEM* fs);
  void saveBootCounter(FILESYSTEM* fs, uint32_t count);
  void notifyClockSynced(const uint8_t* admin_pubkey);
  void addSystemMessage(const char* message);  // Centralised system message creator with boot number
  void loadNetSyncConfig();
  void saveNetSyncConfig();
  void checkNetworkTimeSync();
  void notifyClockSyncedFromRepeaters();

  // Broadcast channel management
  void loadChannelConfig();
  void saveChannelConfig();
  void initialiseChannel();
  void setChannelModePublic();
  void setChannelModePrivate();
  void broadcastBulletin(const char* bulletinText, PostSeverity severity);

  // JSON serial output helpers
  void printJSONSerialLog(const char* component, const char* action, const char* type,
                          const char* severity, const char* text,
                          const uint8_t* user_pubkey, const char* source,
                          uint32_t timestamp);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;

  int calcRxDelay(float score, uint32_t air_time) const override;
  const char* getLogDateTime() override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override ;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len) override;

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {  // v1.11.0
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs, SDStorage* sd = nullptr);

  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }
  ClientACL* getACL() { return &acl; }
  SDStorage* getSDStorage() { return _sd; }
  uint16_t getNumPosted() const { return _num_posted; }

  // Platform-specific file open for writing (used by SystemMessageQueue)
  static File openFileForWrite(FILESYSTEM* fs, const char* filename);

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(uint8_t power_dbm) override;

  void formatNeighborsReply(char *reply) override {
    strcpy(reply, "not supported");
  }

  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  static bool saveFilter(ClientInfo* client);

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply, ClientInfo* client = NULL);
  void addBulletin(const char* bulletinText, PostSeverity severity);  // Server-generated bulletin (serial/UI)
  void notifyUIOfLoadedPosts();  // Push loaded posts to UI after boot
  int getRecentPosts(const PostInfo** dest, int max_posts) const;  // Get latest N posts for display
  bool isDesynced() const;  // Check if RTC clock is desynced (year < 2025)
  void loop();
};

extern MyMesh the_mesh;
