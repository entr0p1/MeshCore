#include "SDStorage.h"

SDStorage::SDStorage() : _status(SD_NOT_SUPPORTED), _cs_pin(-1) {}

bool SDStorage::begin(int cs_pin) {
#if SD_SUPPORTED
  _cs_pin = (cs_pin >= 0) ? cs_pin : PIN_SDCARD_CS;

  if (_cs_pin < 0) {
    Serial.println("SDStorage: No SD CS pin defined for this board");
    _status = SD_NOT_SUPPORTED;
    return false;
  }

  // Configure custom SPI pins if defined (ESP32 only)
#if defined(ESP32) && defined(PIN_SDCARD_SCK) && defined(PIN_SDCARD_MISO) && defined(PIN_SDCARD_MOSI)
  SPI.begin(PIN_SDCARD_SCK, PIN_SDCARD_MISO, PIN_SDCARD_MOSI, _cs_pin);
  Serial.printf("SDStorage: Using custom SPI pins - SCK:%d MISO:%d MOSI:%d CS:%d\n",
                PIN_SDCARD_SCK, PIN_SDCARD_MISO, PIN_SDCARD_MOSI, _cs_pin);
  if (!SD.begin(_cs_pin, SPI)) {
#else
  if (!SD.begin(_cs_pin)) {
#endif
    Serial.println("SDStorage: SD.begin() failed - card not present or unformatted");
    _status = SD_NOT_PRESENT;
    return false;
  }

  // Check if card is actually accessible
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SDStorage: No SD card detected");
    _status = SD_NOT_PRESENT;
    return false;
  }

  // Create base directory if needed
  if (!SD.exists(SD_BASE_DIR)) {
    if (!SD.mkdir(SD_BASE_DIR)) {
      Serial.println("SDStorage: Failed to create bulletin directory");
      _status = SD_UNFORMATTED;
      return false;
    }
  }

  _status = SD_READY;
  Serial.printf("SDStorage: Ready - %lu KB total, %lu KB free\n",
                (unsigned long)getTotalSpace(), (unsigned long)getFreeSpace());
  return true;
#else
  // SD not supported on this platform (PIN_SDCARD_CS not defined)
  _status = SD_NOT_SUPPORTED;
  return false;
#endif
}

uint32_t SDStorage::getTotalSpace() const {
#if SD_SUPPORTED
  if (_status != SD_READY) return 0;
  return SD.totalBytes() / 1024;
#else
  return 0;
#endif
}

uint32_t SDStorage::getUsedSpace() const {
#if SD_SUPPORTED
  if (_status != SD_READY) return 0;
  return SD.usedBytes() / 1024;
#else
  return 0;
#endif
}

uint32_t SDStorage::getFreeSpace() const {
  uint32_t total = getTotalSpace();
  uint32_t used = getUsedSpace();
  return (total > used) ? (total - used) : 0;
}

void SDStorage::formatStorageString(char* dest, size_t len) const {
  switch (_status) {
    case SD_READY: {
      uint32_t used = getUsedSpace();
      uint32_t total = getTotalSpace();
      // Format as KB, MB, or GB depending on size
      if (total >= 1048576) {  // >= 1 GB
        snprintf(dest, len, "%luMB/%luGB",
                 (unsigned long)(used / 1024),
                 (unsigned long)(total / 1048576));
      } else if (total >= 1024) {  // >= 1 MB
        snprintf(dest, len, "%luKB/%luMB",
                 (unsigned long)used,
                 (unsigned long)(total / 1024));
      } else {
        snprintf(dest, len, "%luKB/%luKB",
                 (unsigned long)used, (unsigned long)total);
      }
      break;
    }
    case SD_UNFORMATTED:
      strncpy(dest, "unformatted", len);
      break;
    case SD_NOT_PRESENT:
      strncpy(dest, "no card", len);
      break;
    case SD_NOT_SUPPORTED:
    default:
      strncpy(dest, "Not supported", len);
      break;
  }
  dest[len - 1] = '\0';  // Ensure null termination
}

bool SDStorage::eraseAllData() {
#if SD_SUPPORTED
  if (_status != SD_READY) return false;

  File dir = SD.open(SD_BASE_DIR);
  if (!dir) return false;

  // Delete all files in the directory
  File file = dir.openNextFile();
  while (file) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_BASE_DIR, file.name());
    file.close();
    SD.remove(path);
    file = dir.openNextFile();
  }
  dir.close();

  Serial.println("SDStorage: All data erased");
  return true;
#else
  return false;
#endif
}

void SDStorage::buildPath(char* dest, size_t destLen, const char* filename) const {
  if (filename[0] == '/') {
    snprintf(dest, destLen, "%s%s", SD_BASE_DIR, filename);
  } else {
    snprintf(dest, destLen, "%s/%s", SD_BASE_DIR, filename);
  }
}

#if SD_SUPPORTED
File SDStorage::openForRead(const char* filename) {
  if (_status != SD_READY) return File();
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.open(path, FILE_READ);
}

File SDStorage::openForWrite(const char* filename) {
  if (_status != SD_READY) return File();
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.open(path, FILE_WRITE);
}
#endif

bool SDStorage::exists(const char* filename) {
#if SD_SUPPORTED
  if (_status != SD_READY) return false;
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.exists(path);
#else
  return false;
#endif
}

bool SDStorage::remove(const char* filename) {
#if SD_SUPPORTED
  if (_status != SD_READY) return false;
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.remove(path);
#else
  return false;
#endif
}
