#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "MyMesh.h"
#include "target.h"

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000
#endif
#define BOOT_SCREEN_MILLIS   3000

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

#ifndef BATTERY_MIN_MILLIVOLTS
  #define BATTERY_MIN_MILLIVOLTS 3000
#endif
#ifndef BATTERY_MAX_MILLIVOLTS
  #define BATTERY_MAX_MILLIVOLTS 4200
#endif

#include "icons.h"

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  SplashScreen(UITask* task) : _task(task) {
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    display.setColor(DisplayDriver::BLUE);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    display.setColor(DisplayDriver::LIGHT);
    display.setTextSize(2);
    display.drawTextCentered(display.width()/2, 22, _version_info);

    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 42, FIRMWARE_BUILD_DATE);

    const char* node_type = "< Bulletin Board >";
    display.drawTextCentered(display.width()/2, 54, node_type);

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoStatusScreen();
    }
  }
};

class StatusScreen : public UIScreen {
  UITask* _task;
  NodePrefs* _node_prefs;
  mesh::RTCClock* _rtc;
  int _page_count;

  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
    const int minMilliVolts = BATTERY_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATTERY_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(DisplayDriver::GREEN);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
  }

public:
  StatusScreen(UITask* task, NodePrefs* node_prefs, mesh::RTCClock* rtc)
     : _task(task), _node_prefs(node_prefs), _rtc(rtc), _page_count(1) { }

  void setPageCount(int count) { _page_count = count; }

  int render(DisplayDriver& display) override {
    char tmp[80];

    // Title: "Node Status"
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);
    display.setCursor(0, 0);
    display.print("Node Status");

    // Battery indicator
    renderBatteryIndicator(display, _task->getBattMilliVolts());

    // Page dots - centred at y=12, showing this is page 0
    int y = 12;
    int x_start = (display.width() - (_page_count * 10)) / 2;
    for (int i = 0; i < _page_count; i++) {
      int x = x_start + (i * 10);
      if (i == 0) {  // Current page (status is first)
        display.fillRect(x-1, y-1, 3, 3);
      } else {
        display.fillRect(x, y, 1, 1);
      }
    }

    // Status info
    display.setColor(DisplayDriver::YELLOW);
    display.setTextSize(1);

    // Lines 1-2: Node name (2 lines to prevent wrapping)
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));

    // Calculate max characters per line (128 pixels / 6 pixels per char = 21 chars)
    const int chars_per_line = 21;
    const char* prefix = "Node: ";
    int prefix_len = strlen(prefix);
    int name_len = strlen(filtered_name);

    // Line 1: "Node: <first part>"
    display.setCursor(0, 18);
    if (name_len <= chars_per_line - prefix_len) {
      // Name fits on one line
      sprintf(tmp, "%s%s", prefix, filtered_name);
      display.print(tmp);
    } else {
      // Name needs 2 lines - print first part
      int first_line_chars = chars_per_line - prefix_len;
      sprintf(tmp, "%s%.*s", prefix, first_line_chars, filtered_name);
      display.print(tmp);

      // Line 2: Continue name
      display.setCursor(0, 29);
      display.print(&filtered_name[first_line_chars]);
    }

    // Line 3: ACL counts (moved down to y=40, efficiently count in one pass)
    int admin_count = 0, rw_count = 0, ro_count = 0;
    ClientACL* acl = the_mesh.getACL();
    int num_clients = acl->getNumClients();
    for (int i = 0; i < num_clients; i++) {
      ClientInfo* client = acl->getClientByIdx(i);
      uint8_t role = client->permissions & PERM_ACL_ROLE_MASK;
      if (role == PERM_ACL_ADMIN) admin_count++;
      else if (role == PERM_ACL_READ_WRITE) rw_count++;
      else if (role == PERM_ACL_READ_ONLY) ro_count++;
    }

    display.setCursor(0, 40);
    sprintf(tmp, "ACL: %dA/%dRW/%dR", admin_count, rw_count, ro_count);
    display.print(tmp);

    // Line 4: Clock status with full date/time or NOT SYNCED
    bool synced = !the_mesh.isDesynced();
    display.setCursor(0, 51);
    if (synced) {
      uint32_t current_time = _rtc->getCurrentTime();
      DateTime dt = DateTime(current_time);
      sprintf(tmp, "Clk:%02d/%02d/%02d %02d:%02d",
              dt.day(), dt.month(), dt.year() % 100,
              dt.hour(), dt.minute());
    } else {
      sprintf(tmp, "Clock: NOT SYNCED");
    }
    display.print(tmp);

    return 5000;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _task->gotoRadioConfigScreen();
      return true;
    }
    return false;
  }
};

class RadioConfigScreen : public UIScreen {
  UITask* _task;
  NodePrefs* _node_prefs;
  int _page_count;

  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
    const int minMilliVolts = BATTERY_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATTERY_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(DisplayDriver::GREEN);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
  }

public:
  RadioConfigScreen(UITask* task, NodePrefs* node_prefs)
     : _task(task), _node_prefs(node_prefs), _page_count(1) { }

  void setPageCount(int count) { _page_count = count; }

  int render(DisplayDriver& display) override {
    char tmp[80];

    // Title: "Radio Config"
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);
    display.setCursor(0, 0);
    display.print("Radio Config");

    // Battery indicator
    renderBatteryIndicator(display, _task->getBattMilliVolts());

    // Page dots - centred at y=12, showing this is page 1
    int y = 12;
    int x_start = (display.width() - (_page_count * 10)) / 2;
    for (int i = 0; i < _page_count; i++) {
      int x = x_start + (i * 10);
      if (i == 1) {  // Current page (radio config is second)
        display.fillRect(x-1, y-1, 3, 3);
      } else {
        display.fillRect(x, y, 1, 1);
      }
    }

    // Radio info page
    display.setColor(DisplayDriver::YELLOW);
    display.setTextSize(1);

    display.setCursor(0, 18);
    sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
    display.print(tmp);

    display.setCursor(0, 29);
    sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
    display.print(tmp);

    display.setCursor(0, 40);
    sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
    display.print(tmp);

    display.setCursor(0, 51);
    sprintf(tmp, "Noise: %d", radio_driver.getNoiseFloor());
    display.print(tmp);

    return 5000;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      // Go to first message if there are messages, otherwise back to status
      if (_page_count > 2) {  // More than just status + radio config
        _task->gotoFirstMessage();
        return true;
      } else {
        _task->gotoStatusScreen();
        return true;
      }
    }
    if (c == KEY_ENTER) {
      _task->gotoStatusScreen();
      return true;
    }
    return false;
  }
};

class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  #define MAX_DISPLAY_MSGS  3
  int curr_idx;  // Current message being viewed (0 = newest)

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { curr_idx = 0; }

  int getDisplayCount() {
    const PostInfo* posts[MAX_DISPLAY_MSGS];
    return the_mesh.getRecentPosts(posts, MAX_DISPLAY_MSGS);
  }

  int getPageCount() { return getDisplayCount() + 2; }  // status + radio + messages
  void resetToFirst() { curr_idx = 0; }

  int render(DisplayDriver& display) override {
    char tmp[16];
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);

    // Get recent posts directly from mesh
    const PostInfo* recent_posts[MAX_DISPLAY_MSGS];
    int count = the_mesh.getRecentPosts(recent_posts, MAX_DISPLAY_MSGS);

    if (count == 0 || curr_idx >= count) {
      // No posts or invalid index
      display.drawTextCentered(display.width()/2, display.height()/2, "No posts");
      return 1000;
    }

    // curr_idx=0 shows newest post (recent_posts[0])
    const PostInfo* p = recent_posts[curr_idx];

    // Calculate age, or show NOSYNC if timestamp is in the future (clock not synced)
    uint32_t current_time = _rtc->getCurrentTime();
    if (current_time < p->post_timestamp) {
      // Timestamp is in the future - clock not synced yet
      sprintf(tmp, "NOSYNC");
    } else {
      // Normal case - show relative time
      int secs = current_time - p->post_timestamp;
      if (secs < 60) {
        sprintf(tmp, "%ds", secs);
      } else if (secs < 60*60) {
        sprintf(tmp, "%dm", secs / 60);
      } else {
        sprintf(tmp, "%dh", secs / (60*60));
      }
    }

    // Format author with first 4 bytes of pubkey (8 hex characters)
    char author_name[70];
    sprintf(author_name, "[%02X%02X%02X%02X]",
            p->author.pub_key[0], p->author.pub_key[1],
            p->author.pub_key[2], p->author.pub_key[3]);

    int timestamp_width = display.getTextWidth(tmp);
    int max_origin_width = display.width() - timestamp_width - 2;

    char filtered_origin[70];
    display.translateUTF8ToBlocks(filtered_origin, author_name, sizeof(filtered_origin));
    display.drawTextEllipsized(0, 0, max_origin_width, filtered_origin);

    display.setCursor(display.width() - timestamp_width - 1, 0);
    display.print(tmp);

    // Page dots - centred at y=12
    int y = 12;
    int page_count = getPageCount();
    int x_start = (display.width() - (page_count * 10)) / 2;
    for (int i = 0; i < page_count; i++) {
      int x = x_start + (i * 10);
      if (i == curr_idx + 2) {  // Current message page (offset by status + radio pages)
        display.fillRect(x-1, y-1, 3, 3);
      } else {
        display.fillRect(x, y, 1, 1);
      }
    }

    display.setCursor(0, 16);
    display.setColor(DisplayDriver::LIGHT);
    char filtered_msg[MAX_POST_TEXT_LEN+1];
    display.translateUTF8ToBlocks(filtered_msg, p->text, sizeof(filtered_msg));
    display.printWordWrap(filtered_msg, display.width());

#if AUTO_OFF_MILLIS==0
    return 10000;
#else
    return 1000;
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      curr_idx++;
      if (curr_idx >= getDisplayCount()) {
        // Loop back to status page
        _task->gotoStatusScreen();
      }
      return true;
    }
    if (c == KEY_ENTER) {
      curr_idx = 0;
      _task->gotoStatusScreen();
      return true;
    }
    return false;
  }
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + AUTO_OFF_MILLIS;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif

  _node_prefs = node_prefs;
  if (_display != NULL) {
    _display->turnOn();
  }

  ui_started_at = millis();
  _alert_expiry = 0;

  splash = new SplashScreen(this);
  status_screen = new StatusScreen(this, node_prefs, &rtc_clock);
  radio_config = new RadioConfigScreen(this, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
  setCurrScreen(splash);
}

void UITask::gotoStatusScreen() {
  MsgPreviewScreen* msg_screen = (MsgPreviewScreen*)msg_preview;
  StatusScreen* status = (StatusScreen*)status_screen;

  // Update status screen with current page count
  status->setPageCount(msg_screen->getPageCount());
  setCurrScreen(status_screen);
}

void UITask::gotoRadioConfigScreen() {
  MsgPreviewScreen* msg_screen = (MsgPreviewScreen*)msg_preview;
  RadioConfigScreen* radio = (RadioConfigScreen*)radio_config;

  // Update radio config screen with current page count
  radio->setPageCount(msg_screen->getPageCount());
  setCurrScreen(radio_config);
}

void UITask::gotoFirstMessage() {
  MsgPreviewScreen* msg_screen = (MsgPreviewScreen*)msg_preview;
  msg_screen->resetToFirst();
  setCurrScreen(msg_preview);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strncpy(_alert, text, sizeof(_alert) - 1);
  _alert[sizeof(_alert) - 1] = '\0';  // Ensure null termination
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
  if (t == UIEventType::roomMessage) {
    // New post available - switch to message screen (unless on splash)
    if (curr != splash) {
      MsgPreviewScreen* msg_screen = (MsgPreviewScreen*)msg_preview;
      msg_screen->resetToFirst();
      setCurrScreen(msg_preview);
    }
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      last_led_increment = LED_ON_MILLIS;
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

void UITask::loop() {
  char c = 0;
#if defined(PIN_USER_BTN) || defined(PIN_USER_BTN_ANA)
  int ev = 0;
#endif
#if defined(PIN_USER_BTN)
  ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  ev = analog_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  }
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;
    _next_refresh = 100;
  }

  userLedHandler();

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(DisplayDriver::DARK);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(DisplayDriver::LIGHT);
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;
      } else {
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
    }
#if AUTO_OFF_MILLIS > 0
    if (millis() > _auto_off) {
      _display->turnOff();
    }
#endif
  }

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      #if defined(THINKNODE_M1) || defined(LILYGO_TECHO)
      if (_display != NULL) {
        _display->startFrame();
        _display->setTextSize(2);
        _display->setColor(DisplayDriver::RED);
        _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
        _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
        _display->endFrame();
      }
      #endif
      _board->powerOff();
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();
      c = 0;
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;
    _next_refresh = 0;
  }
  return c;
}

char UITask::handleLongPress(char c) {
  // Post ALARM bulletin
  char alarm_msg[80];
  uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(alarm_msg, "ALARM at %02d:%02d - %d/%d/%d UTC",
          dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());

  the_mesh.addBulletin(alarm_msg);
  showAlert("Alarm posted!", 1000);

  c = 0;
  return c;
}
