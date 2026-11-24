#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

class MyMesh;

class UITask {
  DisplayDriver* _display;
  MyMesh* _mesh;
  unsigned long _next_read, _next_refresh, _auto_off;
  int _prevBtnState;
  NodePrefs* _node_prefs;
  char _version_info[32];
  uint8_t _screen;
  uint16_t _last_num_posted;

  void renderCurrScreen();
  void renderOverview();
  void renderRooms();
  void renderRadio();
  void renderPageNo();
  void wakeUp(uint8_t screen);
public:
  UITask(DisplayDriver& display) : _display(&display) { _next_read = _next_refresh = 0; _mesh = NULL; _screen = 0; _last_num_posted = 0; }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version, MyMesh* mesh = NULL);

  void loop();
};
