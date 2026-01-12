#pragma once

#include <Arduino.h>

// Forward declarations
class MyMesh;
class SDStorage;

/**
 * Firmware CLI - handles bulletin server specific commands
 *
 * Commands handled:
 * - setperm <pubkey_hex> <permissions> - Set ACL permissions
 * - get acl - Display ACL
 * - bulletin.info/warning/critical <text> - Create bulletins
 * - set/get nettime.enable - Network time sync enable
 * - set/get nettime.maxwait - Network time sync max wait
 * - get nettime.status - Network time sync status
 * - set/get channel.mode - Set or get channel mode
 * - login.history - Display login history
 * - appreply <app_name> <pubkey_hex> <text> - Send app response
 * - erase.sdcard - Erase SD card data
 * - get sd.status - Show SD card status and usage
 */
class FirmwareCLI {
public:
  FirmwareCLI(MyMesh* mesh);

  /**
   * Handle a firmware CLI command
   * @param sender_timestamp Timestamp from sender (0 = serial console)
   * @param command The command string
   * @param reply Buffer to write reply message
   * @return true if command was handled, false if not recognized
   */
  bool handleCommand(uint32_t sender_timestamp, char* command, char* reply);

private:
  MyMesh* _mesh;

  // Individual command handlers (return true if handled)
  bool cmdSetPerm(const char* args, char* reply);
  bool cmdGetAcl(char* reply);
  bool cmdBulletin(const char* cmd, char* reply, bool is_serial);
  bool cmdSetNettimeEnable(const char* val, char* reply);
  bool cmdGetNettimeEnable(char* reply);
  bool cmdSetNettimeMaxwait(const char* val, char* reply);
  bool cmdGetNettimeMaxwait(char* reply);
  bool cmdGetNettimeStatus(char* reply);
  bool cmdGetChannelMode(char* reply);
  bool cmdSetChannelMode(const char* val, char* reply);
  bool cmdLoginHistory(char* reply);
  bool cmdAppReply(const char* args, char* reply);
  bool cmdEraseSDCard(char* reply);
  bool cmdGetSDStatus(char* reply);
};
