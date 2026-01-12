#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "MyMesh.h"
#include "DataStore.h"
#include "SDStorage.h"
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

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

// UI Layout constants
#define UI_HEADER_Y         0     // Title row
#define UI_PAGE_DOTS_Y      12    // Page indicator dots
#define UI_CONTENT_START_Y  18    // First content line after page dots
#define UI_LINE_HEIGHT      10    // Spacing between content lines

#ifndef BATTERY_MIN_MILLIVOLTS
  #define BATTERY_MIN_MILLIVOLTS 3000
#endif
#ifndef BATTERY_MAX_MILLIVOLTS
  #define BATTERY_MAX_MILLIVOLTS 4200
#endif

#ifndef MAX_DISPLAY_MSGS
  #define MAX_DISPLAY_MSGS 3  // Posts shown in UI
#endif

#include "icons.h"

// Forward declaration
class HomeScreen;

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

    const char* node_type = "< Bulletin Server >";
    display.drawTextCentered(display.width()/2, 54, node_type);

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  enum HomePage {
    STATUS,
    NODEINFO,
    RADIOINFO,
    ADVERT,
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
    ALARM,
    SHUTDOWN,
    MSG_BASE    // Messages start here (dynamic pages)
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;

  // Deferred action flags (wait for button release)
  bool _shutdown_init;
  bool _advert_init;
  bool _alarm_init;
  bool _gps_toggle_init;

  // Sensor data
  CayenneLPP sensors_lpp;
  int sensors_nb;
  bool sensors_scroll;
  int sensors_scroll_offset;
  unsigned long next_sensors_refresh;

  // Helper: Calculate battery percentage from millivolts
  static int getBatteryPercentage(uint16_t millivolts) {
    int percentage = ((millivolts - BATTERY_MIN_MILLIVOLTS) * 100) /
                     (BATTERY_MAX_MILLIVOLTS - BATTERY_MIN_MILLIVOLTS);
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    return percentage;
  }

  // Helper: Render page indicator dots
  void renderPageDots(DisplayDriver& display, int page_count, int current_page) {
    int x_start = (display.width() - (page_count * 10)) / 2;
    for (int i = 0; i < page_count; i++) {
      int x = x_start + (i * 10);
      if (i == current_page) {
        display.fillRect(x-1, UI_PAGE_DOTS_Y-1, 3, 3);
      } else {
        display.fillRect(x, UI_PAGE_DOTS_Y, 1, 1);
      }
    }
  }

  // Helper: Render battery indicator at top-right of display
  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    int batteryPercentage = getBatteryPercentage(batteryMilliVolts);

    // Battery icon dimensions
    const int iconWidth = 24;
    const int iconHeight = 10;
    const int iconX = display.width() - iconWidth - 5;
    const int iconY = 0;

    display.setColor(DisplayDriver::GREEN);

    // Battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // Battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // Fill the battery based on percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
  }

  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      if (_sensors != NULL) {
        _sensors->querySensors(0xFF, sensors_lpp);
      }
      LPPReader reader(sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
#if AUTO_OFF_MILLIS > 0
      next_sensors_refresh = millis() + 5000;
#else
      next_sensors_refresh = millis() + 60000;
#endif
    }
  }

  int getMessageCount() {
    const PostInfo* posts[MAX_DISPLAY_MSGS];
    return the_mesh.getRecentPosts(posts, MAX_DISPLAY_MSGS);
  }

  int getPageCount() {
    return MSG_BASE + getMessageCount();
  }

  // Page-specific render methods
  void renderStatusPage(DisplayDriver& display) {
    char tmp[80];
    display.setColor(DisplayDriver::YELLOW);
    int y = UI_CONTENT_START_Y;

    // Node name (may contain UTF8, may be long)
    snprintf(tmp, sizeof(tmp), "Node: %s", _node_prefs->node_name);
    char filtered[sizeof(tmp)];
    display.translateUTF8ToBlocks(filtered, tmp, sizeof(filtered));
    display.drawTextEllipsized(0, y, display.width(), filtered);
    y += (UI_LINE_HEIGHT * 2);

    // SD card status
    char sd_str[24];
    SDStorage* sd = the_mesh.getDataStore()->getSD();
    if (sd) {
      sd->formatStorageString(sd_str, sizeof(sd_str));
    } else {
      strncpy(sd_str, "Unsupported", sizeof(sd_str));
      sd_str[sizeof(sd_str) - 1] = '\0';
    }
    snprintf(tmp, sizeof(tmp), "SD: %s", sd_str);
    display.drawTextEllipsized(0, y, display.width(), tmp);
    y += UI_LINE_HEIGHT;

    // Clock status
    if (!the_mesh.isDesynced()) {
      uint32_t current_time = _rtc->getCurrentTime();
      DateTime dt = DateTime(current_time);
      snprintf(tmp, sizeof(tmp), "Clk:%02d/%02d/%02d %02d:%02d",
               dt.day(), dt.month(), dt.year() % 100,
               dt.hour(), dt.minute());
    } else {
      snprintf(tmp, sizeof(tmp), "Clock: NOT SYNCED");
    }
    display.setCursor(0, y);
    display.print(tmp);
  }

  void renderNodeInfoPage(DisplayDriver& display) {
    char tmp[80];
    display.setColor(DisplayDriver::YELLOW);
    int y = UI_CONTENT_START_Y;

    // ACL counts
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
    snprintf(tmp, sizeof(tmp), "ACL: %dA/%dRW/%dR", admin_count, rw_count, ro_count);
    display.setCursor(0, y);
    display.print(tmp);
    y += UI_LINE_HEIGHT;

    // Firmware version
    snprintf(tmp, sizeof(tmp), "FW Version: %s", FIRMWARE_VERSION);
    display.setCursor(0, y);
    display.print(tmp);
    y += UI_LINE_HEIGHT;

    // MeshCore version
    snprintf(tmp, sizeof(tmp), "MC Version: %s", MESHCORE_VERSION);
    display.setCursor(0, y);
    display.print(tmp);
  }

  void renderRadioInfoPage(DisplayDriver& display) {
    char tmp[80];
    display.setColor(DisplayDriver::YELLOW);
    int y = UI_CONTENT_START_Y;

    snprintf(tmp, sizeof(tmp), "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
    display.setCursor(0, y);
    display.print(tmp);
    y += UI_LINE_HEIGHT;

    snprintf(tmp, sizeof(tmp), "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
    display.setCursor(0, y);
    display.print(tmp);
    y += UI_LINE_HEIGHT;

    snprintf(tmp, sizeof(tmp), "TX: %ddBm", _node_prefs->tx_power_dbm);
    display.setCursor(0, y);
    display.print(tmp);
    y += UI_LINE_HEIGHT;

    snprintf(tmp, sizeof(tmp), "Noise: %d", radio_driver.getNoiseFloor());
    display.setCursor(0, y);
    display.print(tmp);
  }

  void renderAdvertPage(DisplayDriver& display) {
    display.setColor(DisplayDriver::GREEN);
    display.setTextSize(1);
    if (_advert_init) {
      display.drawTextCentered(display.width() / 2, 34, "sending...");
    } else {
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
    }
  }

#if ENV_INCLUDE_GPS == 1
  void renderGPSPage(DisplayDriver& display) {
    display.setColor(DisplayDriver::YELLOW);
    char buf[50];
    int y = UI_CONTENT_START_Y;

    bool gps_state = _task->getGPSState();
#ifdef PIN_GPS_SWITCH
    bool hw_gps_state = digitalRead(PIN_GPS_SWITCH);
    if (gps_state != hw_gps_state) {
      strcpy(buf, gps_state ? "gps off(hw)" : "gps off(sw)");
    } else {
      strcpy(buf, gps_state ? "gps on" : "gps off");
    }
#else
    strcpy(buf, gps_state ? "gps on" : "gps off");
#endif
    display.drawTextLeftAlign(0, y, buf);

    LocationProvider* nmea = _sensors != NULL ? _sensors->getLocationProvider() : NULL;
    if (nmea == NULL) {
      y += UI_LINE_HEIGHT;
      display.drawTextLeftAlign(0, y, "Can't access GPS");
    } else {
      strcpy(buf, nmea->isValid() ? "fix" : "no fix");
      display.drawTextRightAlign(display.width()-1, y, buf);
      y += UI_LINE_HEIGHT;
      display.drawTextLeftAlign(0, y, "sat");
      sprintf(buf, "%ld", nmea->satellitesCount());
      display.drawTextRightAlign(display.width()-1, y, buf);
      y += UI_LINE_HEIGHT;
      display.drawTextLeftAlign(0, y, "pos");
      sprintf(buf, "%.4f %.4f",
        nmea->getLatitude()/1000000., nmea->getLongitude()/1000000.);
      display.drawTextRightAlign(display.width()-1, y, buf);
    }

    y = 64 - 11;
    display.setColor(DisplayDriver::GREEN);
    if (_gps_toggle_init) {
      display.drawTextCentered(display.width() / 2, y, "toggling...");
    } else {
      display.drawTextCentered(display.width() / 2, y, "toggle: " PRESS_LABEL);
    }
  }
#endif

#if UI_SENSORS_PAGE == 1
  void renderSensorsPage(DisplayDriver& display) {
    display.setColor(DisplayDriver::YELLOW);
    int y = UI_CONTENT_START_Y;
    refresh_sensors();
    char buf[30];
    char name[30];
    LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

    // Skip to scroll offset
    for (int i = 0; i < sensors_scroll_offset; i++) {
      uint8_t channel, type;
      r.readHeader(channel, type);
      r.skipData(type);
    }

    int display_count = sensors_scroll ? UI_RECENT_LIST_SIZE : sensors_nb;
    for (int i = 0; i < display_count; i++) {
      uint8_t channel, type;
      if (!r.readHeader(channel, type)) {
        r.reset();
        r.readHeader(channel, type);
      }

      display.setCursor(0, y);
      float v;
      switch (type) {
        case LPP_GPS: {
          float lat, lon, alt;
          r.readGPS(lat, lon, alt);
          strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
          break;
        }
        case LPP_VOLTAGE:
          r.readVoltage(v);
          strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
          break;
        case LPP_CURRENT:
          r.readCurrent(v);
          strcpy(name, "current"); sprintf(buf, "%.3f", v);
          break;
        case LPP_TEMPERATURE:
          r.readTemperature(v);
          strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
          break;
        case LPP_RELATIVE_HUMIDITY:
          r.readRelativeHumidity(v);
          strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
          break;
        case LPP_BAROMETRIC_PRESSURE:
          r.readPressure(v);
          strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
          break;
        case LPP_ALTITUDE:
          r.readAltitude(v);
          strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
          break;
        case LPP_POWER:
          r.readPower(v);
          strcpy(name, "power"); sprintf(buf, "%6.2f", v);
          break;
        default:
          r.skipData(type);
          strcpy(name, "unk"); sprintf(buf, "");
      }
      display.setCursor(0, y);
      display.print(name);
      display.drawTextRightAlign(display.width()-1, y, buf);
      y += UI_LINE_HEIGHT;
    }
  }
#endif

  void renderAlarmPage(DisplayDriver& display) {
    display.setColor(DisplayDriver::RED);
    display.setTextSize(1);
    if (_alarm_init) {
      display.drawTextCentered(display.width() / 2, 34, "sending alarm...");
    } else {
      display.drawXbm((display.width() - 32) / 2, 18, alarm_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, 64 - 11, "alarm: " PRESS_LABEL);
    }
  }

  void renderShutdownPage(DisplayDriver& display) {
    display.setColor(DisplayDriver::GREEN);
    display.setTextSize(1);
    if (_shutdown_init) {
      display.drawTextCentered(display.width() / 2, 34, "hibernating...");
    } else {
      display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate: " PRESS_LABEL);
    }
  }

  void renderMessagePage(DisplayDriver& display, int msg_idx) {
    char tmp[24];

    // Get recent posts directly from mesh
    const PostInfo* recent_posts[MAX_DISPLAY_MSGS];
    int count = the_mesh.getRecentPosts(recent_posts, MAX_DISPLAY_MSGS);

    if (count == 0 || msg_idx >= count) {
      display.setColor(DisplayDriver::LIGHT);
      display.drawTextCentered(display.width()/2, display.height()/2, "No posts");
      return;
    }

    const PostInfo* p = recent_posts[msg_idx];

    // Calculate age, or show NOSYNC if timestamp is in the future
    uint32_t current_time = _rtc->getCurrentTime();
    if (current_time < p->post_timestamp) {
      snprintf(tmp, sizeof(tmp), "NOSYNC");
    } else {
      int secs = current_time - p->post_timestamp;
      if (secs < 60) {
        snprintf(tmp, sizeof(tmp), "%ds", secs);
      } else if (secs < 60*60) {
        snprintf(tmp, sizeof(tmp), "%dm", secs / 60);
      } else {
        snprintf(tmp, sizeof(tmp), "%dh", secs / (60*60));
      }
    }

    // Format author with first 4 bytes of pubkey (8 hex characters)
    char author_name[24];
    snprintf(author_name, sizeof(author_name), "[%02X%02X%02X%02X]",
             p->author.pub_key[0], p->author.pub_key[1],
             p->author.pub_key[2], p->author.pub_key[3]);

    // Header: author on left, timestamp on right
    display.setColor(DisplayDriver::GREEN);
    int timestamp_width = display.getTextWidth(tmp);
    int max_origin_width = display.width() - timestamp_width - 2;
    display.drawTextEllipsized(0, UI_HEADER_Y, max_origin_width, author_name);
    display.setCursor(display.width() - timestamp_width - 1, UI_HEADER_Y);
    display.print(tmp);

    display.setCursor(0, UI_CONTENT_START_Y);

    // Set colour based on severity prefix
    if (strncmp(p->text, SEVERITY_PREFIX_CRITICAL, SEVERITY_PREFIX_LEN - 1) == 0) {
      display.setColor(DisplayDriver::RED);
    } else if (strncmp(p->text, SEVERITY_PREFIX_WARNING, SEVERITY_PREFIX_LEN - 1) == 0) {
      display.setColor(DisplayDriver::YELLOW);
    } else {
      display.setColor(DisplayDriver::LIGHT);
    }

    char filtered_msg[MAX_POST_TEXT_LEN+1];
    display.translateUTF8ToBlocks(filtered_msg, p->text, sizeof(filtered_msg));
    display.printWordWrap(filtered_msg, display.width());
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs),
       _page(0), _shutdown_init(false), _advert_init(false), _alarm_init(false),
       _gps_toggle_init(false), sensors_lpp(200), sensors_nb(0), sensors_scroll(false),
       sensors_scroll_offset(0), next_sensors_refresh(0) { }

  void poll() override {
    // Deferred actions - wait for button release
    if (!_task->isButtonPressed()) {
      if (_shutdown_init) {
        _task->shutdown();
      }
      if (_advert_init) {
        the_mesh.sendSelfAdvertisement(0);  // Send immediately
        _task->showAlert("Advert sent!", 1000);
        _advert_init = false;
      }
#if ENV_INCLUDE_GPS == 1
      if (_gps_toggle_init) {
        _task->toggleGPS();
        _gps_toggle_init = false;
      }
#endif
      if (_alarm_init) {
        // Post ALARM bulletin
        char alarm_msg[80];
        uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        snprintf(alarm_msg, sizeof(alarm_msg), "ALARM at %02d:%02d - %d/%d/%d UTC",
                 dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
        the_mesh.addBulletin(alarm_msg, SEVERITY_WARNING);
        _task->showAlert("Alarm posted!", 1000);
        _alarm_init = false;
      }
    }
  }

  int render(DisplayDriver& display) override {
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);

    // Get page count (may change if messages added/removed)
    int page_count = getPageCount();

    // Clamp page if it exceeds bounds
    if (_page >= page_count) {
      _page = 0;
    }

    // Title based on current page
    const char* title;
    if (_page == STATUS) title = "Node Status";
    else if (_page == NODEINFO) title = "Node Info";
    else if (_page == RADIOINFO) title = "Radio Config";
    else if (_page == ADVERT) title = "Send Advert";
#if ENV_INCLUDE_GPS == 1
    else if (_page == GPS) title = "GPS";
#endif
#if UI_SENSORS_PAGE == 1
    else if (_page == SENSORS) title = "Sensors";
#endif
    else if (_page == ALARM) title = "Alarm";
    else if (_page == SHUTDOWN) title = "Shutdown";
    else title = "Message";

    display.setCursor(0, UI_HEADER_Y);
    display.print(title);

    renderBatteryIndicator(display, _task->getBattMilliVolts());
    renderPageDots(display, page_count, _page);

    // Dispatch to page-specific render
    if (_page == STATUS) {
      renderStatusPage(display);
    } else if (_page == NODEINFO) {
      renderNodeInfoPage(display);
    } else if (_page == RADIOINFO) {
      renderRadioInfoPage(display);
    } else if (_page == ADVERT) {
      renderAdvertPage(display);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == GPS) {
      renderGPSPage(display);
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == SENSORS) {
      renderSensorsPage(display);
#endif
    } else if (_page == ALARM) {
      renderAlarmPage(display);
    } else if (_page == SHUTDOWN) {
      renderShutdownPage(display);
    } else if (_page >= MSG_BASE) {
      renderMessagePage(display, _page - MSG_BASE);
    }

    return 5000;
  }

  bool handleInput(char c) override {
    int page_count = getPageCount();

    // Navigation with wrap-around
    if (c == KEY_LEFT || c == KEY_PREV) {
      _page = (_page + page_count - 1) % page_count;
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _page = (_page + 1) % page_count;
      return true;
    }

    // Long-press actions (KEY_ENTER from handleLongPress)
    if (c == KEY_ENTER) {
      if (_page == ADVERT) {
        _advert_init = true;
        return true;
      }
#if ENV_INCLUDE_GPS == 1
      if (_page == GPS) {
        _gps_toggle_init = true;
        return true;
      }
#endif
#if UI_SENSORS_PAGE == 1
      if (_page == SENSORS) {
        // Scroll sensors on long press
        if (sensors_scroll) {
          sensors_scroll_offset = (sensors_scroll_offset + UI_RECENT_LIST_SIZE) % sensors_nb;
        }
        return true;
      }
#endif
      if (_page == ALARM) {
        _alarm_init = true;
        return true;
      }
      if (_page == SHUTDOWN) {
        _shutdown_init = true;
        return true;
      }
    }

    return false;
  }

  void resetToFirstMessage() {
    _page = MSG_BASE;
  }

  void gotoStatusPage() {
    _page = STATUS;
  }
};

// UITask implementation

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
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  setCurrScreen(splash);
}

void UITask::gotoHomeScreen() {
  setCurrScreen(home);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strncpy(_alert, text, sizeof(_alert) - 1);
  _alert[sizeof(_alert) - 1] = '\0';
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
  if (t == UIEventType::roomMessage) {
    // New post available - switch to message screen (unless on splash)
    if (curr != splash) {
      ((HomeScreen*)home)->resetToFirstMessage();
      setCurrScreen(home);
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
  // Pass KEY_ENTER to current screen for page-specific actions
  // Display must be on first
  if (_display != NULL && !_display->isOn()) {
    _display->turnOn();
    _auto_off = millis() + AUTO_OFF_MILLIS;
    _next_refresh = 0;
    return 0;
  }
  return c;
}

bool UITask::isButtonPressed() const {
#if defined(PIN_USER_BTN)
  return user_btn.isPressed();
#elif defined(PIN_USER_BTN_ANA)
  return analog_btn.isPressed();
#else
  return false;
#endif
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return strcmp(_sensors->getSettingValue(i), "1") == 0;
      }
    }
  }
  return false;
}

void UITask::toggleGPS() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        NodePrefs* prefs = the_mesh.getNodePrefs();
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          prefs->gps_enabled = 0;
          showAlert("GPS: Disabled", 800);
        } else {
          _sensors->setSettingValue("gps", "1");
          prefs->gps_enabled = 1;
          showAlert("GPS: Enabled", 800);
        }
        the_mesh.savePrefs();  // Persist to storage
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::shutdown(bool restart) {
  if (restart) {
    _board->reboot();
  } else {
    if (_display != NULL) {
      _display->turnOff();
    }
    radio_driver.powerOff();
    _board->powerOff();
  }
}
