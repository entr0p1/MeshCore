# Bulletin Board Changelog

## v1.0.0 - Beta 1

### Breaking Changes
- `addbulletin` command removed (replaced with bulletin.info/warning/critical)
- Post text length reduced to accommodate severity prefixes

### Planned upcoming breaking changes
N/A at this time.

### New Features

#### Login History Tracking
The server now keeps an in-memory log of recent logins. The log is cleared on reboot.
- Added `login.history` admin CLI command
- Tracks last 5 user logins in memory (not persisted)
- Displays user public key, role, and human-readable timestamp
- Newest logins shown first

#### Bulletin Severity Levels
Bulletins are now broken down into three severities; info, warning, critical. This will help communicate the urgency behind a bulletin, and uses multiple delivery mechanisms for critical (and therefore time-sensitive) bulletins.
- Replaced `addbulletin` command and introduced three severity levels with individual commands to publish:
  - `bulletin.info` - Local post only (delivered to the room only and pushed only to clients that are currently logged in)
  - `bulletin.warning` - Local post + broadcast on channel
  - `bulletin.critical` - Local post + broadcast on channel
- Added severity prefix to all bulletin posts (BLTN-INFO/WARN/CRIT)
- Implemented 10-second rate limiting between bulletin posts

#### Broadcast Channel System
Enables critical bulletins to be duplicated to a channel in addition to the regular room post. This allows time-sensitive bulletins to be delivered to users who may not have an active, logged in session to the server.
- Added public/private channel modes
- Public mode uses the first 32 characters (16 bytes) of the server's public key as the channel hex secret key (open access)
- Private mode uses random 16-byte secret key (restricted access)
- Channel configuration persisted to flash (`/channel_cfg`)
- CLI commands: `channel`, `channel public`, `channel private`
- WARNING and CRITICAL bulletins broadcast to all nodes on channel

#### External Application Integration
Allows developers to interact directly with a MeshCore node via serial interface. This minimises the dependency on middleware applications when creating automations and allows the developer to add new automations without having to hard-code them into firmware. A user sends the `!app` command along with the target app and request data (e.g. `!app weather 3000`) which then prints a JSON output on the serial console detailing the request. The target app can then parse the JSON and provide a response to be delivered to the user.
- User command `!app <app_name> <command>` forwards requests to serial
- JSON request format for external applications (response in JSON from app coming soon)
- CLI command `appreply <app_name> <pubkey_hex> <response_text>`
- 10-second timeout for application responses
- See [external-apps.md](docs/external-apps.md) for full documentation

#### JSON Serial Output
Server events (such as user login and executed commands) are sent to the serial console in JSON format so that they can be parsed by external tools. 
- Structured JSON output for events
- Consistent schema: component/action/data/meta
- Components: post, app, system, login, config
- See [json-output.md](docs/json-output.md) for complete specification

#### User Commands
Users can now send commands in the room with the prefix `!` to perform various actions on the server. All user command interactions are visible only to the user that executed it, and the server admin. While the commands and responses are shown in the room for the user who executed them, they are not visible to other users.
- Added `!help` command system with per-command help
- Added `!version` to display firmware and MeshCore version
- Added `!channel` to query current broadcast channel mode
- Added `!channelkey` to retrieve channel encryption key
- Added `!rxp` and `!txp` to display routing path information
- Added `!app <app_name> <command>` for external application integration

### Bug Fixes
- Fixed buffer overflow in network time sync message formatter (replaced unsafe strcat with snprintf)
- Fixed broadcast bulletin length validation
- Fixed issue where some actions did not send an ACK to the user

### Improvements
- Bulletins can now be posted via admin CLI on-mesh instead of just via serial console
- Removed redundant permission checks for admin CLI - protocol-level filtering (TXT_TYPE_CLI_DATA) is sufficient
- All bulletin server specific commands can now be executed via the MeshCore CLI on-mesh instead of just via serial console
- Consolidated duplicate file-opening functions into single static helper
- Extracted battery rendering code to shared helper functions (removed duplication)

### Technical Changes
- Maximum post length reduced from 151 to 140 characters
- Added `MAX_USER_REPLY_SIZE` and `MAX_CLI_REPLY_SIZE` constants
- Added `LoginHistoryEntry` struct and circular buffer tracking in MyMesh
- Added `trackLogin()` method to record successful logins
