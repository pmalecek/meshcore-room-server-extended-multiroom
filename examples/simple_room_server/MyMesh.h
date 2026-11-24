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
#include <helpers/RegionMap.h>
#include <helpers/RoutingPolicy.h>
#include <RTClib.h>
#include <target.h>

/* ------------------------------ Config -------------------------------- */

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "15 Oct 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.18.0"
#endif

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
  #define  ADVERT_NAME   "Test BBS"
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
  #define MAX_UNSYNCED_POSTS    64
#endif

/* ------------------------------ Sub-rooms ------------------------------ */

#define SUBROOM_NAME_LEN       15
#define SUBROOM_WELCOME_LEN    140
#define SUBROOM_ALL            0xFF      // post_room for system posts sent to every sub-room
#define SUBROOM_FLAG_PREFIX    0x01      // prefix pushed posts with "[room] "
#define SUBROOM_FLAG_DISP_WAKE 0x02      // turn the display on when a new post arrives
#define ROOM_ONLINE_SECS       3600      // client counts as 'online' if active within this time
#define SUBROOMS_FILE          "/s_rooms"

#ifndef SUBROOM_DEFAULT_NAME
  #define SUBROOM_DEFAULT_NAME   "Lobby"
#endif
#ifndef SUBROOM_DEFAULT_WELCOME
  #define SUBROOM_DEFAULT_WELCOME  "Vitej na {name}! Jsi v {room}. Rooms: {rooms}. Prepni: /join <cislo>, /help"
#endif

// texts sent to users (no diacritics on purpose: some device displays lack the glyphs)
#define SUBROOM_TXT_HELP       "Prikazy: /rooms, /join <cislo|nazev>, /exit (zpet do lobby), /where, /help"
#define SUBROOM_TXT_WHERE      "Jsi v room %s"
#define SUBROOM_TXT_JOINED     "Jsi v room %s (%d novych zprav)"
#define SUBROOM_TXT_ALREADY    "Uz jsi v room %s"
#define SUBROOM_TXT_NOT_FOUND  "Room '%s' neexistuje. Seznam: /rooms"

struct SubRoomConfig {
  char names[MAX_SUBROOMS][SUBROOM_NAME_LEN+1];   // empty name = unused slot (slot 0 always used)
  char welcome[SUBROOM_WELCOME_LEN+1];            // template: {name} {room} {rooms}
  uint8_t flags;                                  // SUBROOM_FLAG_*
};

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY   300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY     200
#endif

#define FIRMWARE_ROLE "room_server"

#define PACKET_LOG_FILE  "/packet_log"

#define MAX_POST_TEXT_LEN    (160-9)

struct PostInfo {
  mesh::Identity author;
  uint32_t post_timestamp;   // by OUR clock
  uint8_t  room_id;          // sub-room index, or SUBROOM_ALL
  char text[MAX_POST_TEXT_LEN+1];
};

struct SubRoomStats {
  bool active;
  const char* name;
  int posts;           // posts in buffer
  int clients;         // online clients currently in this room
  uint32_t last_post;  // timestamp of newest post (0 = none)
};

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  bool region_load_active;
  NodePrefs _prefs;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  ClientACL acl;
  CommonCLI _cli;
  unsigned long dirty_contacts_expiry;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  unsigned long next_push;
  uint16_t _num_posted, _num_post_pushes;
  int next_client_idx;  // for round-robin polling
  int next_post_idx;
  PostInfo posts[MAX_UNSYNCED_POSTS];   // cyclic queue
  CayenneLPP telemetry;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];

  SubRoomConfig rooms;

  void addPost(ClientInfo* client, const char* postData);
  void storePost(const mesh::Identity& author, const char* postData, uint8_t room_id);
  void pushPostToClient(ClientInfo* client, PostInfo& post);
  uint8_t getUnsyncedCount(ClientInfo* client);
  bool isPostForClient(const PostInfo& post, const ClientInfo* client) const;

  // sub-rooms
  void loadRooms();
  void saveRooms();
  bool isRoomActive(int idx) const { return idx == 0 || (idx > 0 && idx < MAX_SUBROOMS && rooms.names[idx][0] != 0); }
  bool hasOptionalRooms() const;
  int  findRoom(const char* key) const;
  int  formatRoomList(char* out, int max_len, int mark_room) const;
  void expandWelcome(const ClientInfo* client, char* out, int max_len) const;
  void sendNotice(ClientInfo* client, const char* text);
  static const char* findUserCommand(const char* text);
  void handleUserCommand(ClientInfo* client, const char* command, char* reply);
  bool handleRoomCommand(char* command, char* reply);
  void clearRoomPosts(int idx);
  void applyForceSince(ClientInfo* client, uint32_t force_since);
  bool processAck(const uint8_t *data);
  mesh::Packet* createSelfAdvert();
  File openAppend(const char* fname);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);

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
  bool getCADEnabled() const override {
    return _prefs.cad_enabled;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

  bool allowPacketForward(const mesh::Packet* packet) override;
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override ;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override;

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void addSystemPost(const char* postData, uint8_t room_id = SUBROOM_ALL);

  // for UI / display
  void getRoomStats(int idx, SubRoomStats& out);
  int  getNumBufferedPosts() const;
  int  getNumOnlineClients();
  uint16_t getNumPosted() const { return _num_posted; }
  uint32_t getCurrentTime() { return getRTCClock()->getCurrentTime(); }
  bool isDisplayWakeOn() const { return (rooms.flags & SUBROOM_FLAG_DISP_WAKE) != 0; }

  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  bool setRxBoostedGain(bool enable) override;

  void formatNeighborsReply(char *reply) override {
    strcpy(reply, "not supported");
  }
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  static bool saveFilter(ClientInfo* client);

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();
};
