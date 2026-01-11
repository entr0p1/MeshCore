#pragma once

#include <Arduino.h>
#include <Mesh.h>

// Forward declarations
class MyMesh;
struct ClientInfo;

// Max reply size for user commands
#define MAX_USER_REPLY_SIZE 160

/**
 * User CLI - handles ! prefixed user commands
 *
 * Commands handled:
 * - !help [cmd] - Display help for available commands
 * - !version - Display firmware and MeshCore version info
 * - !channel - Display current broadcast channel mode
 * - !channelkey - Display channel encryption key (hex)
 * - !rxp - Display receive path (route from user to server)
 * - !txp - Display transmit path (route from server to user)
 * - !app <app_name> <command> - Send command to external application
 */
class UserCLI {
public:
  UserCLI(MyMesh* mesh);

  /**
   * Handle a user CLI command (commands starting with !)
   * @param client The client making the request
   * @param packet The received packet (for RX path info)
   * @param command The full command string (including ! prefix)
   * @param reply Buffer to write reply message (MAX_USER_REPLY_SIZE bytes)
   * @return true if command was handled (was a ! command), false if not a user command
   */
  bool handleCommand(ClientInfo* client, mesh::Packet* packet, const char* command, char* reply);

private:
  MyMesh* _mesh;

  // Individual command handlers
  bool cmdHelp(const char* cmd, char* reply);
  bool cmdVersion(char* reply);
  bool cmdChannel(char* reply);
  bool cmdChannelKey(char* reply);
  bool cmdRxPath(mesh::Packet* packet, char* reply);
  bool cmdTxPath(ClientInfo* client, char* reply);
  bool cmdApp(ClientInfo* client, const char* args, char* reply);
};
