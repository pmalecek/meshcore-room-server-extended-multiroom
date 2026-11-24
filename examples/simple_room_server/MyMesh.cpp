#include "MyMesh.h"

#define REPLY_DELAY_MILLIS          1500
#define PUSH_NOTIFY_DELAY_MILLIS    2000
#define SYNC_PUSH_INTERVAL          1200

#define PUSH_ACK_TIMEOUT_FLOOD      12000
#define PUSH_TIMEOUT_BASE           4000
#define PUSH_ACK_TIMEOUT_FACTOR     2000

#define POST_SYNC_DELAY_SECS        6

#define FIRMWARE_VER_LEVEL       1

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
  storePost(client->id, postData, client->extra.room.current_room);
}

void MyMesh::addSystemPost(const char *postData, uint8_t room_id) {
  if (!postData || postData[0] == 0) return;

  MESH_DEBUG_PRINTLN("room.post: addSystemPost: %s", postData);

  storePost(self_id, postData, room_id);
}

void MyMesh::storePost(const mesh::Identity &author, const char *postData, uint8_t room_id) {
  int idx = next_post_idx;
  // TODO: suggested postData format: <title>/<descrption>
  posts[idx].author = author; // add to cyclic queue
  posts[idx].room_id = room_id;
  StrHelper::strncpy(posts[idx].text, postData, MAX_POST_TEXT_LEN);

  posts[idx].post_timestamp = getRTCClock()->getCurrentTimeUnique();
  MESH_DEBUG_PRINTLN("room.post: storePost idx=%d text=%s", idx, posts[idx].text);
  MESH_DEBUG_PRINTLN("room.post: timestamp=%u", posts[idx].post_timestamp);
  next_post_idx = (next_post_idx + 1) % MAX_UNSYNCED_POSTS;

  next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
  _num_posted++; // stats
  MESH_DEBUG_PRINTLN("room.post: next_post_idx=%d num_posted=%d push scheduled", next_post_idx, _num_posted);
}

void MyMesh::pushPostToClient(ClientInfo *client, PostInfo &post) {
  MESH_DEBUG_PRINTLN("room.post: pushPostToClient text=%s", post.text);
  int len = 0;
  memcpy(&reply_data[len], &post.post_timestamp, 4);
  len += 4; // this is a PAST timestamp... but should be accepted by client

  uint8_t attempt;
  getRNG()->random(&attempt, 1); // need this for re-tries, so packet hash (and ACK) will be different
  reply_data[len++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3); // 'signed' plain text

  // encode prefix of post.author.pub_key
  memcpy(&reply_data[len], post.author.pub_key, 4);
  len += 4; // just first 4 bytes

  const char* text = post.text;
  char prefixed[MAX_POST_TEXT_LEN+1];
  if ((rooms.flags & SUBROOM_FLAG_PREFIX) && post.room_id < MAX_SUBROOMS && hasOptionalRooms()) {
    snprintf(prefixed, sizeof(prefixed), "[%s] %s", rooms.names[post.room_id], post.text);
    text = prefixed;
  }
  int text_len = strlen(text);
  memcpy(&reply_data[len], text, text_len);
  len += text_len;

  // calc expected ACK reply
  mesh::Utils::sha256((uint8_t *)&client->extra.room.pending_ack, 4, reply_data, len, client->id.pub_key, PUB_KEY_SIZE);
  client->extra.room.push_post_timestamp = post.post_timestamp;
  client->extra.room.push_room = client->extra.room.current_room;   // ACK advances sync of THIS sub-room

  auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, reply_data, len);
  if (reply) {
    if (client->out_path_len == OUT_PATH_UNKNOWN) {
      unsigned long delay_millis = 0;
      sendFloodScoped(default_scope, reply, delay_millis, _prefs.path_hash_mode + 1); // REVISIT
      client->extra.room.ack_timeout = futureMillis(PUSH_ACK_TIMEOUT_FLOOD);
    } else {
      sendDirect(reply, client->out_path, client->out_path_len);

      uint8_t path_hash_count = client->out_path_len & 63;
      client->extra.room.ack_timeout = futureMillis(PUSH_TIMEOUT_BASE + PUSH_ACK_TIMEOUT_FACTOR * (path_hash_count + 1));
    }
    _num_post_pushes++; // stats
  } else {
    client->extra.room.pending_ack = 0;
    MESH_DEBUG_PRINTLN("Unable to push post to client");
  }
}

bool MyMesh::isPostForClient(const PostInfo& post, const ClientInfo* client) const {
  uint8_t r = client->extra.room.current_room;
  if (r >= MAX_SUBROOMS) r = 0;
  return post.post_timestamp > client->extra.room.sync_since[r]        // is new post for this Client (in its sub-room)?
      && (post.room_id == r || post.room_id == SUBROOM_ALL)          // posted to the Client's sub-room?
      && !post.author.matches(client->id);                            // don't push posts to the author
}

uint8_t MyMesh::getUnsyncedCount(ClientInfo *client) {
  uint8_t count = 0;
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (isPostForClient(posts[k], client)) {
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
      uint8_t r = client->extra.room.push_room < MAX_SUBROOMS ? client->extra.room.push_room : 0;
      client->extra.room.sync_since[r] = client->extra.room.push_post_timestamp; // advance Client's SINCE timestamp (of that sub-room), to sync next post
      return true;
    }
  }
  return false;
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_ROOM, app_data);

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

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload,
                          size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {
    ServerStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
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
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

    // This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

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
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()
      && mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped, _prefs.flood_max_advert)) {
    return false;
  }
  return true;
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
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
    if (data[8] == 0) {   // blank password, just check if sender is in ACL
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

      bool is_new = acl.getClient(sender.pub_key, PUB_KEY_SIZE) == NULL;
      client = acl.putClient(sender, 0);  // add to known clients (if not already known)
      if (sender_timestamp <= client->last_timestamp) {
        MESH_DEBUG_PRINTLN("possible replay attack!");
        return;
      }

      MESH_DEBUG_PRINTLN("Login success!");
      client->last_timestamp = sender_timestamp;
      // NOTE: the companion keeps ONE 'sync since' (newest post received from ANY sub-room),
      //       so the per-room state is kept here, on the server. The client's value is only
      //       used to lower it (e.g. companion was reset and wants the history again).
      for (int r = 0; r < MAX_SUBROOMS; r++) {
        if (is_new || sender_sync_since < client->extra.room.sync_since[r]) {
          client->extra.room.sync_since[r] = sender_sync_since;
        }
      }
      if (is_new) {
        client->extra.room.current_room = 0;                       // newcomers go to the default sub-room
        client->extra.room.flags |= ROOM_FLAG_NEED_WELCOME;
      } else {
        if (!isRoomActive(client->extra.room.current_room)) client->extra.room.current_room = 0;
        if (client->extra.room.current_room != 0) client->extra.room.flags |= ROOM_FLAG_NEED_WHERE;   // remind where they are
      }
      client->extra.room.pending_ack = 0;
      client->extra.room.push_failures = 0;

      client->last_activity = getRTCClock()->getCurrentTime();
      client->permissions &= ~0x03;
      client->permissions |= perm;
      memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }

    if (packet->isRouteFlood()) {
      client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
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
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet *reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, client->shared_secret, reply_data, 13);
      if (reply) {
        if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
          sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
        } else {
          sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
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
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA || flags == TXT_TYPE_CLI_COMMAND)) {
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
      int prefix_len = 0;   // 4 = reply is TXT_TYPE_SIGNED_PLAIN, with author prefix at temp[5]
      bool send_ack;
      if (flags == TXT_TYPE_CLI_DATA || flags == TXT_TYPE_CLI_COMMAND) {
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
      } else if (findUserCommand((const char *)&data[5]) != NULL) { // TXT_TYPE_PLAIN, sub-room command (allowed for guests too)
        // reply as a 'signed' post by the server itself (same as system posts), so apps show it in the room chat
        handleUserCommand(client, findUserCommand((const char *)&data[5]), (char *)&temp[9]);
        temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2);
        memcpy(&temp[5], self_id.pub_key, 4);
        prefix_len = 4;
        send_ack = true;
      } else { // TXT_TYPE_PLAIN
        if ((client->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
          temp[5] = 0;      // no reply
          send_ack = false; // no ACK
        } else {
          if (!is_retry) {
            addPost(client, (const char *)&data[5]);
          }
          temp[5] = 0; // no reply (ACK is enough)
          send_ack = true;
        }
      }

      uint32_t delay_millis;
      if (send_ack) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          mesh::Packet *ack = createAck(ack_hash);
          if (ack) sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
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

      int text_len = strlen((char *)&temp[5 + prefix_len]);
      if (text_len > 0) {
        if (now == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          now++;
        }
        memcpy(temp, &now, 4); // mostly an extra blob to help make packet_hash unique

        // calc expected ACK reply
        // mesh::Utils::sha256((uint8_t *)&expected_ack_crc, 4, temp, 5 + text_len, self_id.pub_key,
        // PUB_KEY_SIZE);

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + prefix_len + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, delay_millis + SERVER_RESPONSE_DELAY, packet->getPathHashSize());
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
          applyForceSince(client, forceSince); // force-update the 'sync since' (of current sub-room, if valid)
        }

        client->extra.room.pending_ack = 0;

        // TODO: Throttle KEEP_ALIVE requests!
        // if client sends too quickly, evict()

        // RULE: only send keep_alive response DIRECT!
        if (client->out_path_len != OUT_PATH_UNKNOWN) {
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
            if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          } else {
            mesh::Packet *reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
            if (reply) {
              if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
                sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
              } else {
                sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
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
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len); // store a copy of path, for sendDirect()
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
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4)
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  _logging = false;
  region_load_active = false;
  set_radio_at = revert_radio_at = 0;
  recv_pkt_region = NULL;

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // off by default, was 10.0
  _prefs.tx_delay_factor = 0.5f; // was 0.25f;
  _prefs.direct_tx_delay_factor = 0.2f; // was zero
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
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')
#ifdef ROOM_PASSWORD
  StrHelper::strncpy(_prefs.guest_password, ROOM_PASSWORD, sizeof(_prefs.guest_password));
#endif

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  next_post_idx = 0;
  next_client_idx = 0;
  next_push = 0;
  memset(posts, 0, sizeof(posts));
  _num_posted = _num_post_pushes = 0;

  memset(&rooms, 0, sizeof(rooms));
  StrHelper::strncpy(rooms.names[0], SUBROOM_DEFAULT_NAME, sizeof(rooms.names[0]));
  StrHelper::strncpy(rooms.welcome, SUBROOM_DEFAULT_WELCOME, sizeof(rooms.welcome));
  rooms.flags = SUBROOM_FLAG_PREFIX | SUBROOM_FLAG_DISP_WAKE;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);

  acl.load(_fs, self_id);
  region_map.load(_fs);
  loadRooms();

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);
  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);

  board.attachDynamicPrefs(_prefs.getCustom());

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);   // send un-scoped
      break;
  }
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

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
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

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
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

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

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
  } else if (strncmp(command, "room.", 5) == 0 && handleRoomCommand(command, reply)) {
    // sub-room admin commands (incl. room.post)
  } else if (memcmp(command, "set repeat on", 13) == 0) {
    strcpy(reply, "ERR - repeat now deprecated");
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

bool MyMesh::saveFilter(ClientInfo* client) {
  return client->isAdmin();    // only save Admins
}

void MyMesh::loop() {
  mesh::Mesh::loop();

  if (millisHasNowPassed(next_push) && acl.getNumClients() > 0) {
    // check for ACK timeouts
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->extra.room.pending_ack && millisHasNowPassed(c->extra.room.ack_timeout)) {
        c->extra.room.push_failures++;
        c->extra.room.pending_ack = 0; // reset  (TODO: keep prev expected_ack's in a list, incase they arrive LATER, after we retry)
        MESH_DEBUG_PRINTLN("pending ACK timed out: push_failures: %d", (uint32_t)c->extra.room.push_failures);
      }
    }
    // check next Round-Robin client, and sync next new post
    auto client = acl.getClientByIdx(next_client_idx);
    bool did_push = false;
    if ((client->extra.room.flags & (ROOM_FLAG_NEED_WELCOME | ROOM_FLAG_NEED_WHERE)) && client->last_activity != 0) {
      // pending sub-room notice (welcome / 'you are in room X'), sent as plain text (doesn't touch sync state)
      char msg[MAX_POST_TEXT_LEN+1];
      if (client->extra.room.flags & ROOM_FLAG_NEED_WELCOME) {
        expandWelcome(client, msg, sizeof(msg));
      } else {
        snprintf(msg, sizeof(msg), SUBROOM_TXT_WHERE, rooms.names[client->extra.room.current_room]);
      }
      client->extra.room.flags &= ~(ROOM_FLAG_NEED_WELCOME | ROOM_FLAG_NEED_WHERE);
      sendNotice(client, msg);
      did_push = true;
    } else if (client->extra.room.pending_ack == 0 && client->last_activity != 0 &&
        client->extra.room.push_failures < 3) { // not already waiting for ACK, AND not evicted, AND retries not max
      MESH_DEBUG_PRINTLN("loop - checking for client %02X", (uint32_t)client->id.pub_key[0]);
      uint32_t now = getRTCClock()->getCurrentTime();
      for (int k = 0, idx = next_post_idx; k < MAX_UNSYNCED_POSTS; k++) {
        auto p = &posts[idx];
        if (now >= p->post_timestamp + POST_SYNC_DELAY_SECS &&
            isPostForClient(*p, client)) {   // new post, in Client's sub-room, not by the Client
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
      // were no unsynced posts for curr client, so process next client much quicker! (in next loop())
      next_push = futureMillis(SYNC_PUSH_INTERVAL / 8);
    }
  }

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs, MyMesh::saveFilter);
    dirty_contacts_expiry = 0;
  }

  // TODO: periodically check for OLD/inactive entries in known_clients[], and evict

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

/* ------------------------------ Sub-rooms ------------------------------ */

#define SUBROOMS_FILE_MAGIC   0x52   // 'R'
#define SUBROOMS_FILE_VER     1
#define REPLY_MAX             (MAX_POST_TEXT_LEN+1)

static bool strEqNoCase(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

// copy next space-delimited token from 'src' into 'dest', returns pointer to the rest (spaces skipped)
static const char* nextToken(const char* src, char* dest, int dest_size) {
  while (*src == ' ') src++;
  int n = 0;
  while (*src && *src != ' ') {
    if (n < dest_size - 1) dest[n++] = *src;
    src++;
  }
  dest[n] = 0;
  while (*src == ' ') src++;
  return src;
}

static bool isValidRoomName(const char* name) {
  int len = strlen(name);
  if (len < 1 || len > SUBROOM_NAME_LEN) return false;
  if (len == 1 && name[0] >= '0' && name[0] <= '9') return false;   // single digits are reserved for room numbers
  for (int i = 0; i < len; i++) {
    if ((unsigned char)name[i] <= ' ' || name[i] == '[' || name[i] == ']') return false;
  }
  return true;
}

void MyMesh::loadRooms() {
  if (!_fs->exists(SUBROOMS_FILE)) return;   // keep defaults
#if defined(RP2040_PLATFORM)
  File file = _fs->open(SUBROOMS_FILE, "r");
#else
  File file = _fs->open(SUBROOMS_FILE);
#endif
  if (!file) return;

  uint8_t hdr[3];
  SubRoomConfig tmp;
  bool ok = file.read(hdr, 3) == 3
         && hdr[0] == SUBROOMS_FILE_MAGIC && hdr[1] == SUBROOMS_FILE_VER && hdr[2] == MAX_SUBROOMS
         && file.read((uint8_t *)&tmp, sizeof(tmp)) == sizeof(tmp);
  file.close();

  if (ok) {
    for (int i = 0; i < MAX_SUBROOMS; i++) tmp.names[i][SUBROOM_NAME_LEN] = 0;   // sanitise
    tmp.welcome[SUBROOM_WELCOME_LEN] = 0;
    if (tmp.names[0][0] == 0) StrHelper::strncpy(tmp.names[0], SUBROOM_DEFAULT_NAME, sizeof(tmp.names[0]));
    rooms = tmp;
  } else {
    MESH_DEBUG_PRINTLN("loadRooms: invalid %s, using defaults", SUBROOMS_FILE);
  }
}

void MyMesh::saveRooms() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(SUBROOMS_FILE);
  File file = _fs->open(SUBROOMS_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = _fs->open(SUBROOMS_FILE, "w");
#else
  File file = _fs->open(SUBROOMS_FILE, "w", true);
#endif
  if (file) {
    uint8_t hdr[3] = { SUBROOMS_FILE_MAGIC, SUBROOMS_FILE_VER, MAX_SUBROOMS };
    file.write(hdr, 3);
    file.write((const uint8_t *)&rooms, sizeof(rooms));
    file.close();
  }
}

bool MyMesh::hasOptionalRooms() const {
  for (int i = 1; i < MAX_SUBROOMS; i++) {
    if (rooms.names[i][0]) return true;
  }
  return false;
}

int MyMesh::findRoom(const char* key) const {
  char k[SUBROOM_NAME_LEN+1];
  nextToken(key, k, sizeof(k));
  if (k[0] == 0) return -1;

  if (k[1] == 0 && k[0] >= '0' && k[0] <= '9') {   // by number
    int idx = k[0] - '0';
    return isRoomActive(idx) ? idx : -1;
  }
  for (int i = 0; i < MAX_SUBROOMS; i++) {          // by name (case insensitive)
    if (isRoomActive(i) && strEqNoCase(rooms.names[i], k)) return i;
  }
  return -1;
}

// "0=Lobby*, 1=Tesla, 2=Opravy"   ('*' marks 'mark_room', -1 = none)
int MyMesh::formatRoomList(char* out, int max_len, int mark_room) const {
  int len = 0;
  out[0] = 0;
  for (int i = 0; i < MAX_SUBROOMS; i++) {
    if (!isRoomActive(i)) continue;
    int n = snprintf(&out[len], max_len - len, "%s%d=%s%s", len ? ", " : "", i, rooms.names[i], i == mark_room ? "*" : "");
    if (n < 0 || n >= max_len - len) { len = max_len - 1; break; }   // truncated
    len += n;
  }
  return len;
}

void MyMesh::expandWelcome(const ClientInfo* client, char* out, int max_len) const {
  uint8_t cur = client->extra.room.current_room < MAX_SUBROOMS ? client->extra.room.current_room : 0;
  char list[MAX_SUBROOMS * (SUBROOM_NAME_LEN + 6) + 1];
  int o = 0;
  const char* t = rooms.welcome;
  while (*t && o < max_len - 1) {
    const char* rep = NULL;
    int skip = 0;
    if (strncmp(t, "{name}", 6) == 0) {
      rep = _prefs.node_name; skip = 6;
    } else if (strncmp(t, "{room}", 6) == 0) {
      rep = rooms.names[cur]; skip = 6;
    } else if (strncmp(t, "{rooms}", 7) == 0) {
      formatRoomList(list, sizeof(list), -1); rep = list; skip = 7;
    }
    if (rep) {
      while (*rep && o < max_len - 1) out[o++] = *rep++;
      t += skip;
    } else {
      out[o++] = *t++;
    }
  }
  out[o] = 0;
}

// message to one client, outside of the post sync (not ACK-tracked, server's sync_since untouched).
// Sent as a 'signed' post by the server itself (like system posts), so apps show it in the room chat.
// NOTE: the companion bumps its own 'sync since' to this timestamp - harmless, as the server keeps
//       per-room sync state and only accepts 'force since' values that match a real post.
void MyMesh::sendNotice(ClientInfo* client, const char* text) {
  uint8_t temp[9 + MAX_POST_TEXT_LEN];
  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &now, 4);
  uint8_t attempt;
  getRNG()->random(&attempt, 1);
  temp[4] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3);
  memcpy(&temp[5], self_id.pub_key, 4);   // author prefix = this server

  int text_len = strlen(text);
  if (text_len > MAX_POST_TEXT_LEN) text_len = MAX_POST_TEXT_LEN;
  memcpy(&temp[9], text, text_len);

  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, temp, 9 + text_len);
  if (pkt) {
    if (client->out_path_len == OUT_PATH_UNKNOWN) {
      sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
    } else {
      sendDirect(pkt, client->out_path, client->out_path_len);
    }
  }
}

// keep-alive 'force since' = newest post the companion has (from ANY sub-room), so only accept it
// if it really is a post of the client's current sub-room
void MyMesh::applyForceSince(ClientInfo* client, uint32_t force_since) {
  uint8_t r = client->extra.room.current_room < MAX_SUBROOMS ? client->extra.room.current_room : 0;
  if (force_since <= client->extra.room.sync_since[r]) return;
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (posts[k].post_timestamp == force_since && (posts[k].room_id == r || posts[k].room_id == SUBROOM_ALL)) {
      client->extra.room.sync_since[r] = force_since;
      return;
    }
  }
}

void MyMesh::clearRoomPosts(int idx) {
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (posts[k].room_id == idx) memset(&posts[k], 0, sizeof(posts[k]));   // timestamp 0 = never pushed
  }
}

// If 'text' is a user command, returns pointer to the command word (after the slash), else NULL.
// Skips leading whitespace and invisible chars some keyboards/apps insert (NBSP, zero-width, BOM),
// accepts '/', JSON-escaped '\\/' and the fullwidth solidus U+FF0F.
const char* MyMesh::findUserCommand(const char* text) {
  const uint8_t* p = (const uint8_t*) text;
  for (;;) {
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
    if (p[0] == 0xC2 && p[1] == 0xA0) { p += 2; continue; }                                   // U+00A0 NBSP
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] >= 0x8B && p[2] <= 0x8F) { p += 3; continue; }   // U+200B..U+200F
    if (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0xA0) { p += 3; continue; }                   // U+2060 word joiner
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; continue; }                   // U+FEFF BOM
    break;
  }
  if (p[0] == '/') return (const char*) (p + 1);
  if (p[0] == '\\' && p[1] == '/') return (const char*) (p + 2);                              // JSON-escaped "\/" (some clients)
  if (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x8F) return (const char*) (p + 3);           // U+FF0F fullwidth '/'
  return NULL;
}

// user commands, sent as normal chat messages starting with '/'  ('command' points AFTER the slash)
void MyMesh::handleUserCommand(ClientInfo* client, const char* command, char* reply) {
  char word[12];
  const char* arg = nextToken(command, word, sizeof(word));
  for (char* c = word; *c; c++) *c = tolower((unsigned char)*c);

  uint8_t cur = client->extra.room.current_room < MAX_SUBROOMS ? client->extra.room.current_room : 0;

  if (strcmp(word, "rooms") == 0 || strcmp(word, "r") == 0) {
    int l = snprintf(reply, REPLY_MAX, "Rooms: ");
    formatRoomList(&reply[l], REPLY_MAX - l, cur);
  } else if (((strcmp(word, "join") == 0 || strcmp(word, "j") == 0) && *arg)
             || strcmp(word, "exit") == 0 || strcmp(word, "leave") == 0) {   // /exit = back to the default room
    int idx = (word[0] == 'j') ? findRoom(arg) : 0;
    if (idx < 0) {
      snprintf(reply, REPLY_MAX, SUBROOM_TXT_NOT_FOUND, arg);
    } else if (idx == cur) {
      snprintf(reply, REPLY_MAX, SUBROOM_TXT_ALREADY, rooms.names[idx]);
    } else {
      client->extra.room.current_room = idx;
      client->extra.room.push_failures = 0;
      next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);   // start syncing the new sub-room soon
      snprintf(reply, REPLY_MAX, SUBROOM_TXT_JOINED, rooms.names[idx], (int)getUnsyncedCount(client));
    }
  } else if (strcmp(word, "where") == 0 || strcmp(word, "w") == 0) {
    snprintf(reply, REPLY_MAX, SUBROOM_TXT_WHERE, rooms.names[cur]);
  } else {
    snprintf(reply, REPLY_MAX, "%s", SUBROOM_TXT_HELP);
  }
}

// admin CLI: room.*   (returns false if not a sub-room command)
bool MyMesh::handleRoomCommand(char* command, char* reply) {
  const char* sub = &command[5];
  char tok[SUBROOM_NAME_LEN+2];

  if (strncmp(sub, "post", 4) == 0 && (sub[4] == 0 || sub[4] == ' ')) {   // room.post [@room] text
    const char* msg = &sub[4];
    while (*msg == ' ') msg++;
    uint8_t target = SUBROOM_ALL;
    if (*msg == '@') {
      msg = nextToken(msg + 1, tok, sizeof(tok));
      int idx = findRoom(tok);
      if (idx < 0) { snprintf(reply, REPLY_MAX, "ERR unknown room"); return true; }
      target = idx;
    }
    if (*msg == 0) {
      snprintf(reply, REPLY_MAX, "ERR empty message");
    } else {
      addSystemPost(msg, target);
      snprintf(reply, REPLY_MAX, "OK");
    }
  } else if (strcmp(sub, "list") == 0) {
    int len = 0;
    for (int i = 0; i < MAX_SUBROOMS && len < REPLY_MAX - 1; i++) {
      if (!isRoomActive(i)) continue;
      int n_posts = 0, n_clients = 0;
      for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
        if (posts[k].post_timestamp && posts[k].room_id == i) n_posts++;
      }
      for (int c = 0; c < acl.getNumClients(); c++) {
        auto cl = acl.getClientByIdx(c);
        if (cl->last_activity && cl->extra.room.current_room == i) n_clients++;
      }
      int n = snprintf(&reply[len], REPLY_MAX - len, "%s%d=%s p%d c%d", len ? ", " : "", i, rooms.names[i], n_posts, n_clients);
      if (n < 0 || n >= REPLY_MAX - len) break;
      len += n;
    }
    if (len < REPLY_MAX - 1) {
      snprintf(&reply[len], REPLY_MAX - len, " | prefix %s, wake %s", (rooms.flags & SUBROOM_FLAG_PREFIX) ? "on" : "off",
               (rooms.flags & SUBROOM_FLAG_DISP_WAKE) ? "on" : "off");
    }
  } else if (strcmp(sub, "last") == 0) {   // room.last  (diagnostics: newest post, first bytes in hex)
    int best = -1;
    for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
      if (posts[k].post_timestamp && (best < 0 || posts[k].post_timestamp > posts[best].post_timestamp)) best = k;
    }
    if (best < 0) {
      snprintf(reply, REPLY_MAX, "no posts");
    } else {
      auto p = &posts[best];
      int len = snprintf(reply, REPLY_MAX, "r%d %02X%02X%02X%02X hex:", p->room_id == SUBROOM_ALL ? -1 : p->room_id,
                         p->author.pub_key[0], p->author.pub_key[1], p->author.pub_key[2], p->author.pub_key[3]);
      int tl = strlen(p->text);
      for (int i = 0; i < tl && i < 12 && len < REPLY_MAX - 3; i++) {
        len += snprintf(&reply[len], REPLY_MAX - len, "%02X", (uint8_t)p->text[i]);
      }
      snprintf(&reply[len], REPLY_MAX - len, " txt:%s", p->text);
    }
  } else if (strncmp(sub, "wake ", 5) == 0) {   // room.wake on|off  (display on new post)
    if (strcmp(&sub[5], "on") == 0) {
      rooms.flags |= SUBROOM_FLAG_DISP_WAKE;
    } else if (strcmp(&sub[5], "off") == 0) {
      rooms.flags &= ~SUBROOM_FLAG_DISP_WAKE;
    } else {
      snprintf(reply, REPLY_MAX, "ERR use: room.wake on|off");
      return true;
    }
    saveRooms();
    snprintf(reply, REPLY_MAX, "OK");
  } else if (strncmp(sub, "add ", 4) == 0) {   // room.add <name>
    nextToken(&sub[4], tok, sizeof(tok));
    if (!isValidRoomName(tok)) {
      snprintf(reply, REPLY_MAX, "ERR name: 1-%d chars, no spaces, not a single digit", SUBROOM_NAME_LEN);
    } else if (findRoom(tok) >= 0) {
      snprintf(reply, REPLY_MAX, "ERR room exists");
    } else {
      int slot = -1;
      for (int i = 1; i < MAX_SUBROOMS; i++) {
        if (rooms.names[i][0] == 0) { slot = i; break; }
      }
      if (slot < 0) {
        snprintf(reply, REPLY_MAX, "ERR no free slot (max %d)", MAX_SUBROOMS - 1);
      } else {
        clearRoomPosts(slot);   // just in case
        StrHelper::strncpy(rooms.names[slot], tok, sizeof(rooms.names[slot]));
        saveRooms();
        snprintf(reply, REPLY_MAX, "OK - room %d = %s", slot, rooms.names[slot]);
      }
    }
  } else if (strncmp(sub, "del ", 4) == 0) {   // room.del <id|name>
    int idx = findRoom(&sub[4]);
    if (idx < 0) {
      snprintf(reply, REPLY_MAX, "ERR unknown room");
    } else if (idx == 0) {
      snprintf(reply, REPLY_MAX, "ERR default room can't be deleted (use room.rename)");
    } else {
      clearRoomPosts(idx);
      for (int c = 0; c < acl.getNumClients(); c++) {   // move its clients to the default room
        auto cl = acl.getClientByIdx(c);
        if (cl->extra.room.current_room == idx) {
          cl->extra.room.current_room = 0;
          cl->extra.room.flags |= ROOM_FLAG_NEED_WHERE;
        }
      }
      rooms.names[idx][0] = 0;
      saveRooms();
      snprintf(reply, REPLY_MAX, "OK - room %d deleted", idx);
    }
  } else if (strncmp(sub, "rename ", 7) == 0) {   // room.rename <id|name> <new name>
    const char* rest = nextToken(&sub[7], tok, sizeof(tok));
    int idx = findRoom(tok);
    char nm[SUBROOM_NAME_LEN+2];
    nextToken(rest, nm, sizeof(nm));
    if (idx < 0) {
      snprintf(reply, REPLY_MAX, "ERR unknown room");
    } else if (!isValidRoomName(nm)) {
      snprintf(reply, REPLY_MAX, "ERR name: 1-%d chars, no spaces, not a single digit", SUBROOM_NAME_LEN);
    } else if (findRoom(nm) >= 0 && findRoom(nm) != idx) {
      snprintf(reply, REPLY_MAX, "ERR room exists");
    } else {
      StrHelper::strncpy(rooms.names[idx], nm, sizeof(rooms.names[idx]));
      saveRooms();
      snprintf(reply, REPLY_MAX, "OK - room %d = %s", idx, rooms.names[idx]);
    }
  } else if (strncmp(sub, "clear ", 6) == 0) {   // room.clear <id|name>
    int idx = findRoom(&sub[6]);
    if (idx < 0) {
      snprintf(reply, REPLY_MAX, "ERR unknown room");
    } else {
      clearRoomPosts(idx);
      snprintf(reply, REPLY_MAX, "OK - room %d cleared", idx);
    }
  } else if (strncmp(sub, "welcome", 7) == 0 && (sub[7] == 0 || sub[7] == ' ')) {   // room.welcome [text|default]
    const char* txt = &sub[7];
    while (*txt == ' ') txt++;
    if (*txt == 0) {
      snprintf(reply, REPLY_MAX, "%s", rooms.welcome);   // show current
    } else {
      if (strcmp(txt, "default") == 0) txt = SUBROOM_DEFAULT_WELCOME;
      bool too_long = strlen(txt) > SUBROOM_WELCOME_LEN;
      StrHelper::strncpy(rooms.welcome, txt, sizeof(rooms.welcome));
      saveRooms();
      snprintf(reply, REPLY_MAX, too_long ? "OK (truncated to %d chars)" : "OK", SUBROOM_WELCOME_LEN);
    }
  } else if (strncmp(sub, "prefix ", 7) == 0) {   // room.prefix on|off
    if (strcmp(&sub[7], "on") == 0) {
      rooms.flags |= SUBROOM_FLAG_PREFIX;
    } else if (strcmp(&sub[7], "off") == 0) {
      rooms.flags &= ~SUBROOM_FLAG_PREFIX;
    } else {
      snprintf(reply, REPLY_MAX, "ERR use: room.prefix on|off");
      return true;
    }
    saveRooms();
    snprintf(reply, REPLY_MAX, "OK");
  } else {
    return false;   // not ours
  }
  return true;
}

/* ------------------------------ UI stats ------------------------------ */

void MyMesh::getRoomStats(int idx, SubRoomStats& out) {
  out.active = isRoomActive(idx);
  out.name = (idx >= 0 && idx < MAX_SUBROOMS) ? rooms.names[idx] : "";
  out.posts = 0;
  out.clients = 0;
  out.last_post = 0;
  if (!out.active) return;

  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    auto p = &posts[k];
    if (p->post_timestamp && (p->room_id == idx || p->room_id == SUBROOM_ALL)) {
      out.posts++;
      if (p->post_timestamp > out.last_post) out.last_post = p->post_timestamp;
    }
  }
  uint32_t now = getRTCClock()->getCurrentTime();
  for (int c = 0; c < acl.getNumClients(); c++) {
    auto cl = acl.getClientByIdx(c);
    if (cl->last_activity && now - cl->last_activity < ROOM_ONLINE_SECS && cl->extra.room.current_room == idx) {
      out.clients++;
    }
  }
}

int MyMesh::getNumBufferedPosts() const {
  int n = 0;
  for (int k = 0; k < MAX_UNSYNCED_POSTS; k++) {
    if (posts[k].post_timestamp) n++;
  }
  return n;
}

int MyMesh::getNumOnlineClients() {
  int n = 0;
  uint32_t now = getRTCClock()->getCurrentTime();
  for (int c = 0; c < acl.getNumClients(); c++) {
    auto cl = acl.getClientByIdx(c);
    if (cl->last_activity && now - cl->last_activity < ROOM_ONLINE_SECS) n++;
  }
  return n;
}
