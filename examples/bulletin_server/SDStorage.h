#pragma once

#include <Arduino.h>

#ifdef PIN_SDCARD_CS
  #include <SD.h>
  #include <SPI.h>
  #include <FS.h>
  #define SD_SUPPORTED 1
#else
  #define SD_SUPPORTED 0
#endif

// SD Card storage status
enum SDStatus {
  SD_NOT_SUPPORTED,    // Platform doesn't support SD (PIN_SDCARD_CS not defined)
  SD_NOT_PRESENT,      // SD slot exists but no card inserted
  SD_UNFORMATTED,      // Card present but not formatted/accessible
  SD_READY             // Card ready for use
};

// Base directory for all bulletin server files on SD card
#define SD_BASE_DIR "/bulletin"

class SDStorage {
public:
  SDStorage();

  // Initialize SD card (call in setup, after board.begin())
  // cs_pin: -1 = use board default from PIN_SDCARD_CS define
  // Returns true if SD card is ready for use
  bool begin(int cs_pin = -1);

  // Status
  SDStatus getStatus() const { return _status; }
  bool isReady() const { return _status == SD_READY; }

  // Storage info (in KB)
  uint32_t getTotalSpace() const;
  uint32_t getUsedSpace() const;
  uint32_t getFreeSpace() const;

  // Format storage string for display
  // Returns: "128KB/32GB", "unformatted", "no card", or "Not supported"
  void formatStorageString(char* dest, size_t len) const;

  // Erase all bulletin server data from SD card
  bool eraseAllData();

#if SD_SUPPORTED
  // File operations (automatically adds /bulletin/ prefix)
  // Only available when PIN_SDCARD_CS is defined
  File openForRead(const char* filename);
  File openForWrite(const char* filename);
#endif

  bool exists(const char* filename);
  bool remove(const char* filename);

private:
  SDStatus _status;
  int _cs_pin;

  // Helper to construct full path with /bulletin/ prefix
  void buildPath(char* dest, size_t destLen, const char* filename) const;
};
