# Bulletin Server Application

A room server that adds a bulletin system where posts can be created via USB serial interface and synchronised to connected clients.

## Features

### Post Management
- **Serial Interface**: Create posts via USB using `addbulletin <text>` command
- **Persistent Storage**: Posts survive reboots via flash storage
- **Circular Buffer**: Stores up to 32 posts (`MAX_UNSYNCED_POSTS`)
- **Auto-Sync**: Connected clients automatically receive new posts
- **Timestamp Preservation**: Stores absolute Unix timestamps (not boot-relative)

### Display and User Interface
- **Splash Screen**: 3-second boot screen with firmware version and server identifier
- **Status Dashboard**: Node name, ACL statistics, date/time, and clock sync status
- **Radio Config**: Frequency, spreading factor, bandwidth, coding rate, TX power, noise floor
- **Message View**: 3 most recent posts with sender IDs and relative timestamps (5s, 3m, 2h ago)
- **Navigation**: Short press cycles screens; long press posts emergency ALARM bulletin
- **Auto-refresh**: Display updates every second

## Usage

### Creating Bulletin Posts

Connect via USB serial (115200 baud) and use:
```
bulletin.info Low battery warning
bulletin.warning Severe weather approaching
bulletin.critical EVACUATION REQUIRED - Move to shelter immediately
```

**Severity Levels:**
- **INFO**: Local post only, delivered to connected clients
- **WARNING**: Local post + broadcast on channel (all nodes receive)
- **CRITICAL**: Local post + broadcast on channel (all nodes receive)

Posts are immediately:
1. Stored in the circular buffer
2. Saved to flash storage
3. Queued for sync to connected clients
4. Broadcast on channel (WARNING/CRITICAL only)
5. Displayed on the UI (if present)

Maximum post length: 140 characters (`MAX_POST_TEXT_LEN`)

### Emergency Alarm

Long press the user button to post an emergency ALARM bulletin with current timestamp:
```
ALARM at 14:23 - 25/10/2025 UTC
```

**Note:** The alarm is stored locally but only delivered to clients with active login sessions. See [Emergency Alarm Feature](#emergency-alarm-feature) in the Permissions section for delivery requirements.

### Navigation

- **Short Press**: Cycle through screens (Status → Radio Config → Messages → Status...)
- **Display Auto-Off**: Screen turns off after 15 seconds of inactivity (`AUTO_OFF_MILLIS`)
- **Wake**: Any button press wakes the display
- **Default Screen**: Status page shown after boot and when returning from messages

## Architecture

### Key Components

#### MyMesh
Room server based mesh implementation handling:
- Post storage and persistence
- Client authentication and ACL management
- Post synchronisation protocol
- Command processing via CommonCLI

#### UITask
Display and input management:
- Screen rendering and transitions
- Button input handling
- Message preview with pagination
- Battery indicator rendering

#### AbstractUITask
Minimal interface between mesh networking and UI:
- Provides battery voltage access
- Receives notification triggers (via `notify()`)
- Handles loop updates

The bulletin server uses a **pull model** - UI queries MyMesh directly via `getRecentPosts()` rather than receiving pushed message data like companion_radio.

## Permissions & Access Control

The bulletin server implements a three-tier permission system:

- **GUEST (0)**: Read-only access (no password, view posts only)
- **READ_WRITE (2)**: Standard users (guest password, can create posts when clock synced)
- **ADMIN (3)**: Full privileges (admin password, CLI commands, clock sync, system messages, persists to ACL)

**Quick Reference:**
```
password <new_password>               # Set admin password
set guest_password <password>         # Set guest password (default: "hello")
set allow_read_only on|off            # Enable/disable passwordless guest access
get acl                                # View admin access list
setperm <pubkey_hex> <permissions>    # Grant permissions (admin only)
```

**Read-Only Mode:** When server clock is desynced (year < 2025), post creation is blocked for all users until an admin syncs the clock.

See [docs/permissions.md](docs/permissions.md) for detailed permission levels, login flow, ACL management, and emergency alarm feature.

## Configuration

### Battery Thresholds
Adjust voltage thresholds for different battery chemistry in `platformio.ini`:
```ini
build_flags =
  -D BATTERY_MIN_MILLIVOLTS=3200  ; Default: 3000
  -D BATTERY_MAX_MILLIVOLTS=4300  ; Default: 4200
```

### Timing Constants
```cpp
AUTO_OFF_MILLIS      15000  // Display auto-off timeout (ms)
BOOT_SCREEN_MILLIS    3000  // Splash screen duration (ms)
LONG_PRESS_MILLIS     1200  // Long press detection threshold (ms)
```

### Storage Limits
```cpp
MAX_UNSYNCED_POSTS      32  // Maximum posts stored
MAX_POST_TEXT_LEN      140  // Maximum user message text length
MAX_DISPLAY_MSGS         3  // Posts shown in UI
```

## User Commands

Connected clients can send commands starting with `!` to query server information and interact with external applications:

- `!help` - List all available user commands
- `!help <command>` - Show detailed help for a specific command
- `!version` - Display firmware, MeshCore version, and server role
- `!channel` - Show current broadcast channel mode (public/private)
- `!channelkey` - Display the 32-character channel encryption key
- `!rxp` - Show receive path routing information (FLOOD/DIRECT)
- `!txp` - Show transmit path routing information (FLOOD/DIRECT)
- `!app <app_name> <command>` - Send command to external application (see [External Applications](docs/external-apps.md))

**Notes:**
- User commands are available to all authenticated users (READ_WRITE and ADMIN)
- Responses appear in the message list with the server's identity
- 10-second timeout for `!app` commands if no response received

## CLI Commands

### Bulletin Server Commands
- `bulletin.info <text>` - Create INFO level bulletin (local only, max 140 chars)
- `bulletin.warning <text>` - Create WARNING level bulletin (local + broadcast, max 140 chars)
- `bulletin.critical <text>` - Create CRITICAL level bulletin (local + broadcast, max 140 chars)

**Serial and Admin access:** Use bulletin commands via serial console or as admin over mesh. Admins receive confirmation responses when posting via mesh (e.g., "OK - WARNING bulletin posted").

**Rate Limiting:** 10-second minimum interval between bulletin posts.

### Broadcast Channel Commands
- `channel` - Show current channel mode (public/private)
- `channel public` - Switch to public mode (uses server's public key)
- `channel private` - Generate new private key and switch to private mode

**Note:** Changing channel mode regenerates the key and saves to flash. Only authorised users on the same channel will receive broadcasts.

### Login History Commands
- `login.history` - Display last 5 user logins with timestamps (admin only)

Shows most recent logins first with format: `[PUBKEY] ROLE - DD/MM/YYYY HH:MM:SS UTC`

### External Application Commands
- `appreply <app_name> <pubkey_hex> <response_text>` - Send application response to user

See [External Applications](docs/external-apps.md) for complete `!app` command documentation.

### Network Time Sync Commands
- `set nettime.enable on|off` - Enable/disable network time synchronisation
- `get nettime.enable` - Show network time sync enabled status
- `set nettime.maxwait <5-60>` - Set agreement window in minutes (default: 15)
- `get nettime.maxwait` - Show current agreement window setting
- `get nettime.status` - Show sync status and repeater count

### CommonCLI Commands
All standard MeshCore CLI commands (reboot, clock, time, password, get, set, etc.) are available.
See the main MeshCore documentation for complete command reference.

## Broadcast Channel

WARNING and CRITICAL bulletins are broadcast to all nodes in the mesh via a shared channel, allowing important alerts to reach everyone (not just connected clients).

**Channel Modes:**
- **Public** (default): Uses server's public key (open access)
- **Private**: Uses random 16-byte secret key (restricted access)

**Quick Reference:**
```
channel               # Show current mode
channel public        # Switch to public mode
channel private       # Switch to private mode (generates new key)
!channelkey           # User command to retrieve channel key
```

See [docs/broadcast-channel.md](docs/broadcast-channel.md) for detailed channel configuration, broadcast behaviour, and storage format.

## File Persistence

All critical data is saved to flash and survives reboots:

- **Posts**: `/posts` - Bulletin messages with author, timestamp, and text
- **System Messages**: `/system_msgs` - Admin notifications with boot sequence tracking
- **Boot Counter**: `/boot_count` - Incremented on each boot for message ordering
- **Channel Config**: `/channel_cfg` - Broadcast channel mode and secret key
- **ACL**: `/acl` - Admin user access list

See [docs/file-persistence.md](docs/file-persistence.md) for detailed storage formats and platform compatibility.

## Clock Synchronisation

The server's RTC resets on every cold boot. When desynced (year < 2025), the server enters **read-only mode** and blocks post creation until the clock is synced.

**Sync Methods (once per boot):**
1. **Admin Auto-Sync**: Clock syncs automatically when admin logs in or sends any message
2. **Network Time Sync**: Syncs from 3 agreeing repeater advertisements (requires `nettime.enable on`)
3. **Manual Sync**: Admin sets clock via `time <unix_timestamp>` or `clock sync` commands

**System Messages:** Admins receive notifications about clock state (boot messages, sync confirmations) via serial and mesh.

**Quick Reference:**
```
time <unix_timestamp>        # Set clock to specific Unix time
clock sync                   # Sync from command packet timestamp
set nettime.enable on|off    # Enable/disable repeater sync (default: off)
set nettime.maxwait <5-60>   # Agreement window in minutes (default: 15)
get nettime.status           # Check sync status and repeater count
```

See [docs/timesync-logic.md](docs/timesync-logic.md) and [docs/system-messages.md](docs/system-messages.md) for complete technical details.

## Platform Support

Tested on:
- **ESP32**: LilyGo T3-S3 with SX1262

Untested:
- **NRF52**
- **RP2040**
- **STM32**

Each platform uses appropriate filesystem operations for file I/O.
