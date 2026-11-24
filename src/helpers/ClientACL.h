#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <helpers/IdentityStore.h>

#define PERM_ACL_ROLE_MASK     3   // lower 2 bits
#define PERM_ACL_GUEST         0
#define PERM_ACL_READ_ONLY     1
#define PERM_ACL_READ_WRITE    2
#define PERM_ACL_ADMIN         3

#define OUT_PATH_UNKNOWN  0xFF

#ifndef MAX_SUBROOMS
  #define MAX_SUBROOMS   4    // room server: sub-room 0 = default, 1..3 optional
#endif

// room server: extra.room.flags
#define ROOM_FLAG_NEED_WELCOME   0x01   // send welcome notice (new client)
#define ROOM_FLAG_NEED_WHERE     0x02   // send 'you are in room X' notice

struct ClientInfo {
  mesh::Identity id;
  uint8_t permissions;
  uint8_t out_path_len;
  uint8_t out_path[MAX_PATH_SIZE];
  uint8_t shared_secret[PUB_KEY_SIZE];
  uint32_t last_timestamp;   // by THEIR clock  (transient)
  uint32_t last_activity;    // by OUR clock    (transient)
  union  {
    struct {
      // sync messages SINCE this timestamp (by OUR clock), one per sub-room.
      // NOTE: sync_since[0] stays at offset 0 (same bytes as legacy 'sync_since', persisted by load()/save())
      uint32_t sync_since[MAX_SUBROOMS];
      uint32_t pending_ack;
      uint32_t push_post_timestamp;
      unsigned long ack_timeout;
      uint8_t  push_failures;
      uint8_t  current_room;   // sub-room the client is currently in
      uint8_t  push_room;      // sub-room of the post awaiting ACK
      uint8_t  flags;          // ROOM_FLAG_*
    } room;
    struct {
      uint32_t expiry_timestamp;  // epoch seconds
      uint32_t push_tag;
      uint16_t scope_region_id;  // scope to use when sending telemetry to this client/subscriber
      uint8_t  min_deltas_len;
      uint8_t  min_deltas[14];  // LPP encoded
      uint8_t  prev_telem[14];  // LPP encoded
    } sensor;
  } extra;
  
  bool isAdmin() const { return (permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_ADMIN; }
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

class ClientACL {
  FILESYSTEM* _fs;
  ClientInfo clients[MAX_CLIENTS];
  int num_clients;

public:
  ClientACL() { 
    memset(clients, 0, sizeof(clients));
    num_clients = 0;
  }
  void load(FILESYSTEM* _fs, const mesh::LocalIdentity& self_id);
  void save(FILESYSTEM* _fs, bool (*filter)(ClientInfo*)=NULL);
  bool clear();

  ClientInfo* getClient(const uint8_t* pubkey, int key_len);
  ClientInfo* putClient(const mesh::Identity& id, uint8_t init_perms);
  bool applyPermissions(const mesh::LocalIdentity& self_id, const uint8_t* pubkey, int key_len, uint8_t perms);

  int getNumClients() const { return num_clients; }
  ClientInfo* getClientByIdx(int idx) { return &clients[idx]; }
};
