# Bulletin Board Application

A room server clone that adds a bulletin board system where posts can be created via USB serial interface and synchronised to connected clients.

## Features

### Post Management
- **Serial Interface**: Create posts via USB using `addbulletin <text>` command
- **Persistent Storage**: Posts survive reboots via flash storage
- **Circular Buffer**: Stores up to 32 posts (`MAX_UNSYNCED_POSTS`)
- **Auto-Sync**: Connected clients automatically receive new posts
- **Timestamp Preservation**: Stores absolute Unix timestamps (not boot-relative)

### User Interface
- **Splash Screen**: 3-second boot screen with firmware info and bulletin board identifier
- **Status Page**: Node name (2 lines), ACL statistics, current date/time, and clock sync status
- **Radio Config**: Radio parameters (frequency, spreading factor, bandwidth, coding rate, TX power, noise floor)
- **Message View**: Shows 3 most recent posts with sender IDs (8-character hex format)
- **Page Navigation**: Short press cycles through screens (Status → Radio Config → Messages)
- **Emergency Alert**: Long press posts ALARM bulletin

### Display Features
- **Clock Sync Detection**: Shows "Clock: NOT SYNCED" when RTC not synced from client
- **Relative Timestamps**: Displays time elapsed (5s, 3m, 2h ago) on message pages
- **Sender Identification**: All messages show sender ID in 8-character hex format [XXXXXXXX]
- **Page Dots**: Visual indicators showing current screen position (centred)
- **Auto-Refresh**: Display updates every 1000ms
- **Status Dashboard**: Node name with word wrapping, ACL entry counts (Admins/Read-Write/Read-only), and full date/time display

## Usage

### Creating Bulletin Posts

Connect via USB serial (115200 baud) and use:
```
addbulletin Hello world!
```

Posts are immediately:
1. Stored in the circular buffer
2. Saved to flash storage
3. Queued for sync to connected clients
4. Displayed on the UI (if present)

Maximum post length: 151 characters (`MAX_POST_TEXT_LEN`)

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

The bulletin board uses a **pull model** - UI queries MyMesh directly via `getRecentPosts()` rather than receiving pushed message data like companion_radio.

## Permissions & Access Control

The bulletin board implements a three-tier permission system to control what users can do.

### Permission Levels

#### **GUEST (0)** - Read-Only Access

**How to get:** No password provided AND `allow_read_only` preference enabled

**Capabilities:**
- ✅ Login and connect to server
- ✅ Receive and view all posts
- ❌ Cannot create posts
- ❌ Cannot send CLI commands
- ❌ No sensor telemetry access (non-admin sensors filtered)
- ❌ Not saved to ACL (doesn't persist across reboots)

**Use case:** Public read-only access for monitoring bulletins without posting rights.

---

#### **READ_WRITE (2)** - Standard User

**How to get:** Use the guest/room password (configured via `ROOM_PASSWORD` or `set guest_password`)

**Capabilities:**
- ✅ Everything GUEST can do, plus:
- ✅ **Create posts** (when clock is synced)
- ❌ Cannot send CLI commands over mesh
- ❌ Cannot sync server clock
- ❌ No full sensor telemetry access
- ❌ Cannot receive system messages
- ❌ Not saved to ACL (doesn't persist across reboots)

**Use case:** Standard users who can contribute bulletins but don't need administrative access.

**Important:** If server clock is desynced, posting is blocked with error: `"Error: Server clock desynced"`

---

#### **ADMIN (3)** - Full Privileges

**How to get:** Use the admin password (configured via `ADMIN_PASSWORD` or `set password`)

**Capabilities:**
- ✅ Everything READ_WRITE can do, plus:
- ✅ **Send CLI commands** over mesh (full control)
- ✅ **Sync server clock** automatically on first login/message
- ✅ **Receive system messages** (clock sync notifications, boot messages)
- ✅ **Full sensor telemetry** access (0xFF permission mask)
- ✅ **Query access list** (view other admins)
- ✅ **Saved to ACL** - persists across reboots

**Use case:** Server administrators who need full control and clock synchronisation privileges.

**System Messages Examples:**
```
SYSTEM: Server rebooted. Clock desynced - read-only until admin login.
SYSTEM: Clock synced by admin [A1B2C3D4]. Server now in read-write mode.
```

---

### Permission Comparison Table

| Feature | GUEST (0) | READ_WRITE (2) | ADMIN (3) |
|---------|-----------|----------------|-----------|
| **View posts** | ✅ | ✅ | ✅ |
| **Create posts** | ❌ | ✅* | ✅* |
| **CLI commands** | ❌ | ❌ | ✅ |
| **Sync clock** | ❌ | ❌ | ✅ |
| **System messages** | ❌ | ❌ | ✅ |
| **Full telemetry** | ❌ | ❌ | ✅ |
| **Query ACL** | ❌ | ❌ | ✅ |
| **Persist to flash** | ❌ | ❌ | ✅ |

*Blocked when server clock is not yet synced

---

### Password Configuration

**Set admin password** (via serial CLI):
```
password <new_password>
```

**Set guest/room password** (via serial CLI):
```
set guest_password <password>
```

**Default values** (set in `MyMesh.h`):
```cpp
#define ADMIN_PASSWORD    "password"      // Change this!
#ifdef ROOM_PASSWORD
  #define ROOM_PASSWORD   "<your_room_pw>" // Optional
#endif
```

**Enable/disable guest read-only access:**
```
set allow_read_only on   # Allow passwordless guest access
set allow_read_only off  # Require guest password or ACL entry
```

---

### Login Flow

When a client connects, the server checks credentials in this order:

1. **Admin password match** → Grant `ADMIN` permissions
2. **Guest password match** → Grant `READ_WRITE` permissions
3. **No password + allow_read_only=true** → Grant `GUEST` permissions
4. **No password + allow_read_only=false** → Check ACL, reject if not found
5. **Wrong password** → Reject connection (no response, client times out)

---

### Access Control List (ACL)

Only **ADMIN** users are saved to the ACL and persist across reboots:

**View current ACL** (via serial):
```
get acl
```

**Output format:**
```
ACL:
03 A1B2C3D4E5F6...  (Admin)
03 C9D0E1F2A3B4...  (Admin)
```

**Set permissions manually** (admin-only via mesh):
```
setperm <pubkey_hex> <permissions>
```

Example:
```
setperm A1B2C3D4E5F6... 3  # Grant admin (saves to ACL)
```

**Notes:**
- GUEST and READ_WRITE users are NOT saved to ACL
- ACL entries persist in flash at `/acl` file
- Only admins can modify ACL via `setperm` command

---

### Clock-Based Restrictions

The bulletin board enforces **read-only mode** when the server's RTC is desynced (year < 2025):

**Blocked operations:**
- ❌ `addbulletin` command (serial) - returns error
- ❌ Post creation (READ_WRITE and ADMIN) - returns error message
- ❌ Alarm button posts (long press) - silently blocked (TODO: Add error message to UI)

### Emergency Alarm Feature

**Long press** the user button to post an emergency ALARM bulletin:

```
ALARM at 14:23 - 25/10/2025 UTC
```

**Permission requirements:**
- ✅ Works from local UI (no login needed)
- ✅ Server creates bulletin with its own identity
- ⚠️ **Requires active client sessions** to receive the alarm
- ⚠️ Clients must be logged in with `last_activity != 0`
- ⚠️ Behaves like regular bulletin (not system message)

**Important:** The alarm bulletin is delivered to **logged-in users only**. If no clients are connected, the alarm is stored locally but won't reach anyone until they log in and sync.

---

### Differences from simple_room_server

bulletin_board extends simple_room_server with additional features:

| Feature | simple_room_server | bulletin_board |
|---------|-------------------|----------------|
| **Permission model** | ✅ Same (Guest/RW/Admin) | ✅ Same |
| **Clock sync** | ❌ None | ✅ Admin/Repeater sync |
| **Read-only mode** | ❌ None | ✅ When desynced |
| **System messages** | ❌ None | ✅ Admin-only notifications |
| **Flash persistence** | ❌ RAM only | ✅ Posts + config saved |
| **Server bulletins** | ❌ Client-only | ✅ Serial + UI button |
| **Posting restrictions** | ✅ Always works | ⚠️ Blocked when desynced |

The **core permission behaviour** is identical, but bulletin_board adds clock-based restrictions and admin-privileged system features.

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
MAX_POST_TEXT_LEN      151  // Maximum post text length (160-9)
MAX_DISPLAY_MSGS         3  // Posts shown in UI
```

## CLI Commands

### Bulletin Board Commands
- `addbulletin <text>` - Create new bulletin post (max 151 chars)

### Network Time Sync Commands
- `set nettime.enable on|off` - Enable/disable network time synchronisation
- `get nettime.enable` - Show network time sync enabled status
- `set nettime.maxwait <5-60>` - Set agreement window in minutes (default: 15)
- `get nettime.maxwait` - Show current agreement window setting
- `get nettime.status` - Show sync status and repeater count

### CommonCLI Commands
All standard MeshCore CommonCLI commands are available:
- `reboot` - Restart the device
- `advert` - Send advertisement immediately
- `clock` - Display current RTC time
- `time <epoch>` - Set RTC time (Unix timestamp)
- `neighbors` - List neighbouring nodes
- `neighbor.remove <pubkey>` - Remove neighbour from table
- `tempradio <freq> <bw> <sf> <cr> <timeout>` - Temporarily change radio parameters
- `password <new_password>` - Set admin password
- `clear stats` - Reset statistics counters
- `get <config>` - Get configuration value (e.g., `get name`, `get radio`)
- `set <config> <value>` - Set configuration value

See `CommonCLI` documentation for complete command reference.

## File Persistence

### Posts Storage

Posts are saved to flash in `/posts` file with this format:
```
[version:1][next_post_idx:4][post_1][post_2]...[post_32]
```

Each post contains:
- Author public key (32 bytes)
- Post timestamp (4 bytes, Unix time)
- Text length (1 byte)
- Text content (variable, max 151 bytes)

**Note**: System messages (timestamp=0) are NOT saved to the posts file.

### System Messages Storage

System messages are stored separately in `/system_msgs`:
```
[num_messages:1][message_1][message_2]...[message_8]
```

Each system message contains:
- Text (152 bytes)
- Boot sequence (4 bytes)
- Created millis (4 bytes)
- Delivered-to tracking (MAX_CLIENTS * 6 bytes)

### Boot Counter

Boot sequence tracking stored in `/boot_count`:
- Single uint32_t value
- Incremented on each boot
- Used for system message ordering

The persistence implementation uses platform-specific file operations to ensure compatibility across NRF52, RP2040, STM32, and ESP32 platforms.

## Clock Synchronisation

The bulletin board server's Real-Time Clock (RTC) resets on every cold boot, losing track of current time. Accurate timekeeping is critical for post ordering and synchronisation.

### Automatic Clock Sync

The server automatically synchronises its clock from admin users through multiple trigger points:

1. **Server boots** with desynced clock
2. **Admin connects** - clock syncs immediately on login from admin logon packet timestamp
3. **Admin activity** - any message, command, or interaction triggers sync (if not already synced)
4. **Server transitions** from read-only mode to normal operation

The clock syncs **once per boot** from the first valid admin timestamp (≥ January 1, 2025).

### Read-Only Mode When Desynced

To prevent creating posts with invalid timestamps, the server blocks bulletin and message creation while desynced:

- **Bulletin creation blocked**: `addbulletin` returns "Error: Clock not synced"
- **Mesh posts blocked**: Remote users receive "Error: Server clock desynced"

### System Messages

Admin users receive system notifications about clock state, printed both to **serial console** and delivered as **mesh messages**:

#### On Boot (if desynced)
**Serial Output**:
```
SYSTEM: Server rebooted. Clock desynced - read-only until admin login.
```

**Mesh Message** (delivered to admins):
```
SYSTEM: Server rebooted. Clock desynced - read-only until admin login.
```

#### After Automatic Sync (from mesh)
**Serial Output**:
```
SYSTEM: Clock synced by admin [A1B2C3D4]. Server now in read-write mode.
```

**Mesh Message** (delivered to admins):
```
SYSTEM: Clock synced by admin [A1B2C3D4]. Server now in read-write mode.
```

#### After Manual Sync (via CLI)
**Serial Output**:
```
SYSTEM: Clock synced manually. Server now in read-write mode.
```

**Mesh Message** (delivered to admins):
```
SYSTEM: Clock synced manually. Server now in read-write mode.
```

#### System Message Behavior

These notifications:
- **Serial console**: Printed immediately when generated
- **Mesh delivery**: Sent to admin users only (not guests)
- **Persistence**: Stored until delivered to all current admins
- **Display filtering**: Hidden from bulletin board's local UI (timestamp=0 marker)
- **Ordering**: Tracked with boot sequence counter for proper sequencing across reboots
- **One-per-admin**: Each admin receives each message exactly once

### Manual Clock Setting

If automatic sync is unavailable, admins can manually set the clock via serial or mesh CLI:

**Set to specific Unix timestamp**:
```
time <unix_timestamp>
```
Example: `time 1735689600` sets clock to January 1, 2025 00:00:00 UTC

**Sync from sender's timestamp**:
```
clock sync
```
Uses the timestamp from the command packet (works over mesh for admins)

Both methods trigger the system message notification and serial console output.

### Network Time Synchronisation

When no admin is available to sync the clock, the bulletin board can automatically synchronise from repeater advertisements in the mesh network. This provides a fallback mechanism for unattended operation.

#### How It Works

1. **Repeater Discovery**: The server listens for advertisements from repeater nodes (ADV_TYPE_REPEATER)
2. **Timestamp Collection**: Collects timestamps from 3 unique repeaters
3. **Agreement Validation**: Ensures all 3 timestamps agree within a configurable window (default: 15 minutes)
4. **Time Selection**: Uses the most recent timestamp from the quorum
5. **One-Time Sync**: Syncs once per boot, just like admin sync

#### Requirements

- **3 repeater adverts** with valid timestamps (≥ Jan 1, 2025)
- All timestamps must be within **maxwait** minutes of each other (default: 15 minutes)
- Feature must be **enabled** (default: off, must be manually enabled)
- Clock must be **desynced** and not yet synced via admin

#### Priority

Admin sync always takes precedence:
- If admin syncs first, repeater adverts are ignored
- If repeaters sync first, admin can still override manually if needed
- Only one sync occurs per boot (admin or repeater, whichever happens first)

#### Configuration

**Enable/disable network time sync**:
```
set nettime.enable on
set nettime.enable off
get nettime.enable
```

**Configure agreement window** (5-60 minutes):
```
set nettime.maxwait 15
get nettime.maxwait
```

**Check sync status**:
```
get nettime.status
```

Returns:
- `Waiting for repeaters (2/3)` - Still collecting adverts
- `Clock already synced` - Clock has been synced this boot
- `Network time sync disabled` - Feature is turned off

#### System Message

When network time sync succeeds, admins receive a detailed notification:

**Serial Output**:
```
SYSTEM: Clock set by Repeater advert from [A1B2C3D4] to 01 Jan 2025 10:30. Quorum nodes: [A1B2C3D4], [E5F6A7B8], [C9D0E1F2].
```

**Mesh Message** (delivered to admins):
```
SYSTEM: Clock set by Repeater advert from [A1B2C3D4] to 01 Jan 2025 10:30. Quorum nodes: [A1B2C3D4], [E5F6A7B8], [C9D0E1F2].
```

The message shows:
- Which repeater's timestamp was used (most recent)
- The exact date/time set
- All 3 repeaters that formed the quorum (for audit trail)

#### Security Considerations

- **Cryptographic signatures**: All repeater adverts are cryptographically signed and verified
- **Quorum requirement**: Requires 3 independent repeaters to agree (prevents single malicious node)
- **Timestamp validation**: Rejects timestamps before Jan 1, 2025
- **Agreement window**: Tight tolerance (15 min default) catches misconfigured or malicious repeaters
- **Monotonic enforcement**: Clock never moves backward
- **One-time sync**: Prevents time drift attacks across multiple syncs

#### When Network Sync Fails

Network sync may not occur if:
- Fewer than 3 repeaters are visible in the mesh
- Repeater timestamps disagree by more than maxwait
- All repeater timestamps are invalid (< Jan 1, 2025)
- Feature is disabled via `set nettime.enable off`

In these cases, manual admin sync is required.

### Technical Details

For comprehensive documentation on the clock synchronisation system, including:
- Desync detection logic
- Boot counter and message ordering
- System message queue architecture
- Security considerations
- Edge case handling

See **[timesync-logic.md](timesync-logic.md)** for complete technical details.

## Platform Support

Tested on:
- **ESP32**: LilyGo T3-S3 with SX1262

Untested:
- **NRF52**
- **RP2040**
- **STM32**

Each platform uses appropriate filesystem operations for file I/O.
