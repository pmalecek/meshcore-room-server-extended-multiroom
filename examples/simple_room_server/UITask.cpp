#include "UITask.h"
#include <Arduino.h>
#include <helpers/CommonCLI.h>
#include "MyMesh.h"

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#define AUTO_OFF_MILLIS      20000  // 20 seconds
#define BOOT_SCREEN_MILLIS   4000   // 4 seconds

// 'meshcore', 128x13px
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe, 
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc, 
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00, 
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00, 
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0, 
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00, 
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00, 
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8, 
};

// screens (cycled by the user button)
#define SCREEN_OVERVIEW   0
#define SCREEN_ROOMS      1
#define SCREEN_RADIO      2
#define NUM_SCREENS       3

#define RECENT_POST_SECS  60   // room row marked with '>' if it got a post within this time

// UTF-8 -> plain ASCII for the built-in display font (Czech + a few German letters, others -> '?')
static void toDisplayAscii(char* dest, const char* src, size_t dest_size) {
  static const struct { uint16_t cp; char a; } map[] = {
    {0xE1,'a'},{0x10D,'c'},{0x10F,'d'},{0xE9,'e'},{0x11B,'e'},{0xED,'i'},{0x148,'n'},{0xF3,'o'},
    {0x159,'r'},{0x161,'s'},{0x165,'t'},{0xFA,'u'},{0x16F,'u'},{0xFD,'y'},{0x17E,'z'},
    {0xC1,'A'},{0x10C,'C'},{0x10E,'D'},{0xC9,'E'},{0x11A,'E'},{0xCD,'I'},{0x147,'N'},{0xD3,'O'},
    {0x158,'R'},{0x160,'S'},{0x164,'T'},{0xDA,'U'},{0x16E,'U'},{0xDD,'Y'},{0x17D,'Z'},
    {0xE4,'a'},{0xF6,'o'},{0xFC,'u'},{0xC4,'A'},{0xD6,'O'},{0xDC,'U'},{0xDF,'s'},
  };
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dest_size - 1; ) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x80) {
      dest[j++] = (c >= 32 && c <= 126) ? c : '?';
      i++;
      continue;
    }
    // decode UTF-8 sequence
    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    i++;
    while (extra-- > 0 && src[i] && ((unsigned char)src[i] & 0xC0) == 0x80) {
      cp = (cp << 6) | ((unsigned char)src[i] & 0x3F);
      i++;
    }
    char a = '?';
    for (size_t k = 0; k < sizeof(map)/sizeof(map[0]); k++) {
      if (map[k].cp == cp) { a = map[k].a; break; }
    }
    dest[j++] = a;
  }
  dest[j] = 0;
}

// "-", "12s", "5m", "3h", "2d"
static void formatAge(char* out, size_t size, uint32_t now, uint32_t ts) {
  if (ts == 0) { snprintf(out, size, "-"); return; }
  uint32_t d = now > ts ? now - ts : 0;
  if (d < 60) snprintf(out, size, "%us", (unsigned)d);
  else if (d < 3600) snprintf(out, size, "%um", (unsigned)(d / 60));
  else if (d < 86400) snprintf(out, size, "%uh", (unsigned)(d / 3600));
  else snprintf(out, size, "%ud", (unsigned)(d / 86400));
}

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version, MyMesh* mesh) {
  _prevBtnState = HIGH;
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _node_prefs = node_prefs;
  _mesh = mesh;
  _screen = SCREEN_OVERVIEW;
  _last_num_posted = _mesh ? _mesh->getNumPosted() : 0;
  _display->turnOn();

  // strip off dash and commit hash by changing dash to null terminator
  // e.g: v1.2.3-abcdef -> v1.2.3
  char *version = strdup(firmware_version);
  char *dash = strchr(version, '-');
  if(dash){
    *dash = 0;
  }

  // v1.2.3 (1 Jan 2025)
  snprintf(_version_info, sizeof(_version_info), "%s (%s)", version, build_date);
  free(version);
}

void UITask::renderPageNo() {
  char tmp[8];
  snprintf(tmp, sizeof(tmp), "%d/%d", _screen + 1, NUM_SCREENS);
  _display->drawTextRightAlign(_display->width(), 56, tmp);
}

void UITask::renderOverview() {
  char tmp[40], name[24];

  toDisplayAscii(name, _node_prefs->node_name, sizeof(name));
  _display->setCursor(0, 0);
  _display->print(name);

  if (_mesh == NULL) return;
  uint32_t now = _mesh->getCurrentTime();

  _display->setCursor(0, 14);
  snprintf(tmp, sizeof(tmp), "Online: %d", _mesh->getNumOnlineClients());
  _display->print(tmp);

  _display->setCursor(0, 24);
  snprintf(tmp, sizeof(tmp), "Zpravy: %d/%d", _mesh->getNumBufferedPosts(), MAX_UNSYNCED_POSTS);
  _display->print(tmp);

  // newest post over all rooms
  SubRoomStats st;
  uint32_t newest = 0;
  int newest_room = -1, n_rooms = 0;
  for (int i = 0; i < MAX_SUBROOMS; i++) {
    _mesh->getRoomStats(i, st);
    if (!st.active) continue;
    n_rooms++;
    if (st.last_post > newest) { newest = st.last_post; newest_room = i; }
  }
  char age[8];
  formatAge(age, sizeof(age), now, newest);
  _display->setCursor(0, 34);
  if (newest_room >= 0) {
    _mesh->getRoomStats(newest_room, st);
    toDisplayAscii(name, st.name, 8);   // max 7 chars, line fits 21 chars
    snprintf(tmp, sizeof(tmp), "Posledni: %s %s", age, name);
  } else {
    snprintf(tmp, sizeof(tmp), "Posledni: -");
  }
  _display->print(tmp);

  _display->setCursor(0, 44);
  snprintf(tmp, sizeof(tmp), "Rooms: %d", n_rooms);
  _display->print(tmp);
}

void UITask::renderRooms() {
  // columns (6px font): marker+no+name | posts | clients | age
  const int x_posts = 80, x_clients = 98, x_age = _display->width();
  char tmp[24], name[12];

  _display->setCursor(0, 0);
  _display->print("Room");
  _display->drawTextRightAlign(x_posts, 0, "zp");
  _display->drawTextRightAlign(x_clients, 0, "kl");
  _display->drawTextRightAlign(x_age, 0, "akt");
  _display->fillRect(0, 9, _display->width(), 1);

  if (_mesh == NULL) return;
  uint32_t now = _mesh->getCurrentTime();

  int y = 12;
  SubRoomStats st;
  for (int i = 0; i < MAX_SUBROOMS; i++) {
    _mesh->getRoomStats(i, st);
    if (!st.active) continue;

    bool recent = st.last_post && now >= st.last_post && now - st.last_post < RECENT_POST_SECS;
    toDisplayAscii(name, st.name, 9);    // max 8 chars, so it ends before the posts column
    snprintf(tmp, sizeof(tmp), "%c%d %s", recent ? '>' : ' ', i, name);
    _display->setCursor(0, y);
    _display->print(tmp);

    snprintf(tmp, sizeof(tmp), "%d", st.posts);
    _display->drawTextRightAlign(x_posts, y, tmp);
    snprintf(tmp, sizeof(tmp), "%d", st.clients);
    _display->drawTextRightAlign(x_clients, y, tmp);
    formatAge(tmp, sizeof(tmp), now, st.last_post);
    _display->drawTextRightAlign(x_age, y, tmp);
    y += 10;
  }
}

void UITask::renderRadio() {
  char tmp[40], name[24];
  toDisplayAscii(name, _node_prefs->node_name, sizeof(name));
  _display->setCursor(0, 0);
  _display->print(name);

  _display->setCursor(0, 20);
  snprintf(tmp, sizeof(tmp), "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
  _display->print(tmp);

  _display->setCursor(0, 30);
  snprintf(tmp, sizeof(tmp), "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
  _display->print(tmp);

  _display->setCursor(0, 40);
  snprintf(tmp, sizeof(tmp), "TX: %d dBm", _node_prefs->tx_power_dbm);
  _display->print(tmp);
}

void UITask::renderCurrScreen() {
  if (millis() < BOOT_SCREEN_MILLIS) { // boot screen
    // meshcore logo
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    uint16_t websiteWidth = _display->getTextWidth(website);
    _display->setCursor((_display->width() - websiteWidth) / 2, 22);
    _display->print(website);

    // version info
    _display->setTextSize(1);
    uint16_t versionWidth = _display->getTextWidth(_version_info);
    _display->setCursor((_display->width() - versionWidth) / 2, 35);
    _display->print(_version_info);

    // node type
    const char* node_type = "< Room Server >";
    uint16_t typeWidth = _display->getTextWidth(node_type);
    _display->setCursor((_display->width() - typeWidth) / 2, 48);
    _display->print(node_type);
  } else {
    _display->setTextSize(1);
    _display->setColor(UIColor::primary_txt);
    switch (_screen) {
      case SCREEN_ROOMS: renderRooms(); break;
      case SCREEN_RADIO: renderRadio(); break;
      default:           renderOverview(); break;
    }
    renderPageNo();
  }
}

void UITask::wakeUp(uint8_t screen) {
  if (!_display->isOn()) _display->turnOn();
  _screen = screen;
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _next_refresh = 0;   // redraw now
}

void UITask::loop() {
#ifdef PIN_USER_BTN
  if (millis() >= _next_read) {
    int btnState = digitalRead(PIN_USER_BTN);
    if (btnState != _prevBtnState) {
      if (btnState == USER_BTN_PRESSED) {  // pressed?
        if (_display->isOn()) {
          _screen = (_screen + 1) % NUM_SCREENS;   // next screen
          _next_refresh = 0;
        } else {
          _display->turnOn();
        }
        _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
      }
      _prevBtnState = btnState;
    }
    _next_read = millis() + 200;  // 5 reads per second
  }
#endif

  // new post -> show room activity (if enabled by 'room.wake on')
  if (_mesh) {
    uint16_t n = _mesh->getNumPosted();
    if (n != _last_num_posted) {
      _last_num_posted = n;
      if (_mesh->isDisplayWakeOn() && millis() >= BOOT_SCREEN_MILLIS) {
        wakeUp(SCREEN_ROOMS);
      }
    }
  }

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      _display->startFrame();
      renderCurrScreen();
      _display->endFrame();

      _next_refresh = millis() + 1000;   // refresh every second
    }
    if (millis() > _auto_off) {
      _display->turnOff();
    }
  }
}
