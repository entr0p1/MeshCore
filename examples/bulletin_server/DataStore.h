#pragma once

#include <helpers/IdentityStore.h>
#include "SDStorage.h"

class DataStore {
  FILESYSTEM* _fs;
  SDStorage* _sd;
  IdentityStore identity_store;

public:
  DataStore(FILESYSTEM& fs);
  void begin(SDStorage* sd = nullptr);

  // Filesystem access
  FILESYSTEM* getFS() const { return _fs; }
  SDStorage* getSD() const { return _sd; }
  bool formatFileSystem();

  // Identity management
  bool loadMainIdentity(mesh::LocalIdentity& identity);
  bool saveMainIdentity(const mesh::LocalIdentity& identity);

  // Platform-agnostic file operations
  File openRead(const char* filename);
  File openWrite(const char* filename);
  File openAppend(const char* filename);
  bool exists(const char* filename);
  bool removeFile(const char* filename);

  // Storage info (in KB)
  uint32_t getStorageUsedKb() const;
  uint32_t getStorageTotalKb() const;

  // SD backup operations - returns false silently if SD not ready
  bool backupToSD(const char* filename);
  bool restoreFromSD(const char* filename);
};
