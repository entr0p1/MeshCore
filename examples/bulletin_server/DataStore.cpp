#include <Arduino.h>
#include "DataStore.h"

// Platform-specific filesystem includes
#if defined(ESP32)
  #include <SPIFFS.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#endif

DataStore::DataStore(FILESYSTEM& fs) : _fs(&fs), _sd(nullptr),
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    identity_store(fs, "")
#elif defined(RP2040_PLATFORM)
    identity_store(fs, "/identity")
#else
    identity_store(fs, "/identity")
#endif
{
}

void DataStore::begin(SDStorage* sd) {
  _sd = sd;
#if defined(RP2040_PLATFORM)
  identity_store.begin();
#endif
}

bool DataStore::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return ((fs::SPIFFSFS *)_fs)->format();
#else
  return false;
#endif
}

bool DataStore::loadMainIdentity(mesh::LocalIdentity& identity) {
  return identity_store.load("_main", identity);
}

bool DataStore::saveMainIdentity(const mesh::LocalIdentity& identity) {
  return identity_store.save("_main", identity);
}

File DataStore::openRead(const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(filename, FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "r");
#else
  return _fs->open(filename, "r", false);
#endif
}

File DataStore::openWrite(const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _fs->remove(filename);
  return _fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "w");
#else
  return _fs->open(filename, "w", true);
#endif
}

File DataStore::openAppend(const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "a");
#else
  return _fs->open(filename, "a", true);
#endif
}

bool DataStore::exists(const char* filename) {
  return _fs->exists(filename);
}

bool DataStore::removeFile(const char* filename) {
  return _fs->remove(filename);
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
static uint32_t _TotalBlocks = 0;

int _countLfsBlock(void *p, lfs_block_t block) {
  if (block > _TotalBlocks) {
    return LFS_ERR_CORRUPT;
  }
  lfs_size_t *size = (lfs_size_t*) p;
  *size += 1;
  return 0;
}

lfs_ssize_t _getLfsUsedBlockCount(FILESYSTEM* fs) {
  lfs_size_t size = 0;
  int err = lfs_traverse(fs->_getFS(), _countLfsBlock, &size);
  if (err) {
    return 0;
  }
  return size;
}
#endif

uint32_t DataStore::getStorageUsedKb() const {
#if defined(ESP32)
  return SPIFFS.usedBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.usedBytes = 0;
  _fs->info(info);
  return info.usedBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  if (_TotalBlocks == 0) {
    _TotalBlocks = _fs->_getFS()->cfg->block_count;
  }
  const lfs_config* config = _fs->_getFS()->cfg;
  int usedBlockCount = _getLfsUsedBlockCount(_fs);
  int usedBytes = config->block_size * usedBlockCount;
  return usedBytes / 1024;
#else
  return 0;
#endif
}

uint32_t DataStore::getStorageTotalKb() const {
#if defined(ESP32)
  return SPIFFS.totalBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.totalBytes = 0;
  _fs->info(info);
  return info.totalBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  const lfs_config* config = _fs->_getFS()->cfg;
  int totalBytes = config->block_size * config->block_count;
  return totalBytes / 1024;
#else
  return 0;
#endif
}

// SD backup operations
bool DataStore::backupToSD(const char* filename) {
  if (!_sd || !_sd->isReady()) return false;
  if (!_fs->exists(filename)) return false;

#if SD_SUPPORTED
  File src = openRead(filename);
  if (!src) return false;

  _sd->remove(filename);
  File dst = _sd->openForWrite(filename);
  if (!dst) {
    src.close();
    return false;
  }

  uint8_t buffer[128];
  bool ok = true;
  while (true) {
    int bytes_read = src.read(buffer, sizeof(buffer));
    if (bytes_read <= 0) break;
    if (dst.write(buffer, bytes_read) != (size_t)bytes_read) {
      ok = false;
      break;
    }
  }

  src.close();
  dst.close();
  return ok;
#else
  return false;
#endif
}

bool DataStore::restoreFromSD(const char* filename) {
  if (!_sd || !_sd->isReady()) return false;
  if (!_sd->exists(filename)) return false;

#if SD_SUPPORTED
  File src = _sd->openForRead(filename);
  if (!src) return false;

  File dst = openWrite(filename);
  if (!dst) {
    src.close();
    return false;
  }

  uint8_t buffer[128];
  bool ok = true;
  while (true) {
    int bytes_read = src.read(buffer, sizeof(buffer));
    if (bytes_read <= 0) break;
    if (dst.write(buffer, bytes_read) != (size_t)bytes_read) {
      ok = false;
      break;
    }
  }

  src.close();
  dst.close();
  return ok;
#else
  return false;
#endif
}
