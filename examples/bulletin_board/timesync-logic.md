# Clock Synchronisation Logic

## Overview

The bulletin board server's Real-Time Clock (RTC) resets to a default value (typically year 2000 or 1970) on every cold boot, losing track of the current date and time. Since all bulletin posts are timestamped using the RTC, accurate timekeeping is critical for proper message ordering and synchronisation.

This document describes the automatic clock synchronisation system that ensures the server obtains accurate time from trusted admin users on the mesh network.

---

## Why Clock Synchronisation is Needed

### The Problem
- **RTC resets on boot**: ESP32, nRF52, and similar microcontrollers do not have battery-backed RTCs
- **Posts need timestamps**: Every bulletin post is stamped with `getCurrentTimeUnique()` from the RTC
- **Ordering matters**: Posts are synced to clients in chronological order based on timestamps
- **Invalid timestamps break sync**: If the clock shows year 2000, all posts get incorrect timestamps

### The Solution
- **Multiple automatic sync sources**:
  - **Admin sync**: When an admin user sends any packet (login, message, command), their timestamp is used to set the server's clock
  - **Network sync**: When no admin available, server automatically syncs from repeater advertisements (requires 3 repeaters with agreeing timestamps)
  - **Manual sync**: CLI commands allow manual intervention
- **One-time sync per boot**: Clock syncs once, preventing time regression attacks
- **Read-only mode when desynced**: Server blocks bulletin creation until clock is synced
- **System messages notify admins**: Admins receive notifications about desync state, sync source, and when sync succeeds
- **Configurable network sync**: Tolerance window and enable/disable controls via CLI

---

## Desync Detection

### How It Works

```cpp
bool MyMesh::isDesynced() const {
  return getRTCClock()->getCurrentTime() < MIN_VALID_TIMESTAMP;
}
```

**Threshold**: `MIN_VALID_TIMESTAMP = 1735689600` (January 1, 2025 00:00:00 UTC)

### Logic
- If RTC time is **before** January 1, 2025 → **desynced**
- If RTC time is **on or after** January 1, 2025 → **synced**

### Why This Works
- MeshCore bulletin board deployments began in 2025
- Any clock showing a date before 2025 is clearly wrong
- Simple integer comparison, no complex date parsing
- No false positives in production use

---

## Boot Behaviour

### 1. Boot Counter Initialisation

On every boot, the server:
1. Loads `/boot_count` from flash (uint32_t)
2. Increments the counter: `current_boot_sequence++`
3. Saves the new value back to flash

**Purpose**: Provides total ordering of system messages across reboots, even when the RTC is desynced.

### 2. System Message Queue Loading

The server loads `/system_msgs` from flash, which contains:
- Previously created system messages
- Per-admin delivery tracking (which admins have received each message)
- Boot sequence and millis timestamps for ordering

### 3. Desync Check and Notification

```cpp
if (isDesynced()) {
  system_msgs->addMessage(
    "SYSTEM: Server rebooted. Clock desynced - read-only until admin login.",
    current_boot_sequence
  );
  system_msgs->save(_fs);
}
```

**Result**: A system message is queued for delivery to all admin users, notifying them of the desync state.

---

## Clock Synchronisation Mechanism

### Multiple Sync Entry Points

Clock synchronisation can be triggered through **four different pathways**, ensuring the server can obtain accurate time from multiple sources:

#### 1. Admin Login (`onAnonDataRecv`)

Triggers when admin authenticates with correct password:

```cpp
// In onAnonDataRecv() - handles initial authentication
if (perm == PERM_ACL_ADMIN && isDesynced() && !clock_synced_once) {
  if (sender_timestamp >= MIN_VALID_TIMESTAMP) {
    getRTCClock()->setCurrentTime(sender_timestamp);
    notifyClockSynced(sender.pub_key);
  }
}
```

**When**: Admin logs in from companion app
**Advantage**: Immediate sync, no additional action needed

#### 2. Admin Messages/Commands (`onPeerDataRecv`)

Triggers when admin sends any text message or CLI command over mesh:

```cpp
// In onPeerDataRecv() - handles all messages after login
if (isDesynced() && !clock_synced_once && client->isAdmin()) {
  if (sender_timestamp >= MIN_VALID_TIMESTAMP) {
    getRTCClock()->setCurrentTime(sender_timestamp);
    notifyClockSynced(client->id.pub_key);
  }
}
```

**When**: Admin sends message or command
**Advantage**: Catches cases where login timestamp was invalid

#### 3. Manual CLI Commands (`handleCommand`)

Triggers when someone manually sets the clock via serial or mesh CLI:

```cpp
// In handleCommand() - detects clock changes from CommonCLI
bool was_desynced = isDesynced();
_cli.handleCommand(sender_timestamp, command, reply);  // Execute "clock sync" or "time <epoch>"

if (was_desynced && !isDesynced()) {
  notifyClockSynced(NULL);  // NULL = manual sync
}
```

**When**: `clock sync` or `time <epoch>` command executed
**Advantage**: Works for manual intervention scenarios

#### 4. Network Time Synchronisation (`onAdvertRecv`)

Triggers when repeater advertisements are received from mesh infrastructure nodes:

```cpp
// In onAdvertRecv() - handles repeater advertisements
void MyMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                          const uint8_t* app_data, size_t app_data_len) {
  // Only process if clock not synced yet
  if (clock_synced_once || !isDesynced() || !netsync_config.enabled) {
    return;  // Already synced, clock functional, or feature disabled
  }

  // Parse advert type - only accept repeater adverts
  AdvertDataParser parser(app_data, app_data_len);
  if (parser.getType() != ADV_TYPE_REPEATER) {
    return;  // Not a repeater
  }

  // Validate timestamp
  if (timestamp < MIN_VALID_TIMESTAMP) {
    return;  // Too old
  }

  // Collect timestamps from 3 unique repeaters
  // Check for agreement within configurable window (default: 15 minutes)
  // Use most recent timestamp from quorum
  checkNetworkTimeSync();
}
```

**When**: Repeater advertisements received automatically (typically every 1-12 hours)
**Advantage**: Fully automatic sync when no admin is available
**Requirements**:
- 3 unique repeater advertisements
- All timestamps must agree within configurable tolerance window (5-60 minutes, default 15)
- Repeater advertisements must be cryptographically verified (handled by base Mesh class)

**How Network Sync Works**:
1. **Collection Phase**: Server collects timestamps from repeater advertisements
2. **Buffering**: Stores up to 3 unique repeaters (identified by first 4 bytes of pub_key)
3. **Agreement Validation**: When 3 repeaters collected, checks if all timestamps are within `maxwait` window
4. **Timestamp Selection**: Uses the **most recent** timestamp from the quorum (not median)
5. **Monotonic Check**: Ensures selected timestamp is greater than current time (if current time is valid)
6. **Sync**: Sets RTC clock and marks `clock_synced_once = true`

**Configuration** (persistent to flash):
- `set/get nettime.enable on|off` - Enable/disable network sync (default: off)
- `set/get nettime.maxwait 5-60` - Agreement tolerance window in minutes (default: 15)
- `get nettime.status` - Check sync status

**System Message** (on success):
```
"Clock set by Repeater advert from [A1B2C3D4] to 01 Jan 2025 10:30. Quorum nodes: [A1B2C3D4], [E5F6A7B8], [C9D0E1F2]."
```

### Sync Priority and Interaction

The four sync pathways interact with specific priority rules:

**Priority Order**:
1. **Admin sync always takes precedence**: When admin syncs (login, message, or CLI), the repeater buffer is immediately cleared
2. **Race condition behaviour**: If admin and repeater both available, whichever syncs *first* wins due to `clock_synced_once` flag
3. **Network sync as fallback**: Network sync only runs when `clock_synced_once == false`, making it a fallback mechanism

**Buffer Clearing on Admin Sync**:
```cpp
// Both admin sync paths clear repeater state
getRTCClock()->setCurrentTime(sender_timestamp);
notifyClockSynced(sender.pub_key);

// Clear repeater buffer - admin sync takes precedence
repeater_count = 0;
check_netsync_flag = false;
```

**Result**: Admin sync is always preferred, but network sync provides automatic fallback when no admin is available.

### Common Conditions (Admin-Based Sync)

The first three pathways (admin login, admin messages, manual CLI) check the same conditions:

1. **Currently desynced**: `isDesynced()` returns true
2. **Not yet synced this boot**: `clock_synced_once == false` (class member)
3. **Sender is admin**: `client->isAdmin()` or `perm == PERM_ACL_ADMIN`
4. **Valid timestamp**: `sender_timestamp >= MIN_VALID_TIMESTAMP` (≥ Jan 1, 2025)

### Unified Notification Handler

All pathways call the same `notifyClockSynced()` helper method:

```cpp
void MyMesh::notifyClockSynced(const uint8_t* admin_pubkey) {
  if (!clock_synced_once) {
    clock_synced_once = true;

    char sync_msg[MAX_POST_TEXT_LEN + 1];
    if (admin_pubkey) {
      sprintf(sync_msg, "SYSTEM: Clock synced by admin [%02X%02X%02X%02X]. Server now in read-write mode.",
              admin_pubkey[0], admin_pubkey[1], admin_pubkey[2], admin_pubkey[3]);
    } else {
      strcpy(sync_msg, "SYSTEM: Clock synced manually. Server now in read-write mode.");
    }

    system_msgs->addMessage(sync_msg, current_boot_sequence);
    system_msgs->save(_fs);
    Serial.println(sync_msg);  // Print to serial console immediately
  }
}
```

### What Happens
1. **RTC is updated**: Clock jumps from ~year 2000 to current time
2. **Sync flag set**: `clock_synced_once = true` prevents future syncs (class member, shared across all pathways)
3. **Success message queued**: System message generated with admin ID (or "manually" for CLI)
4. **Serial output**: Message printed to console immediately
5. **Mesh delivery**: Message queued for delivery to all admins
6. **Operations unblocked**: `isDesynced()` now returns false, allowing bulletin creation

### Sync Priority

If multiple admins attempt to sync simultaneously (unlikely), the **first valid timestamp wins** due to the `clock_synced_once` flag. Subsequent attempts are ignored.

---

## Operational Restrictions

### While Desynced

The server enforces read-only mode to prevent creating posts with invalid timestamps.

#### Serial `addbulletin` Command Blocked

```cpp
if (isDesynced()) {
  strcpy(reply, "Error: Clock not synced");
  return;
}
```

**User Experience**: Admin attempting to post via serial sees error message.

#### Mesh Client Posts Blocked

```cpp
if (isDesynced()) {
  strcpy((char*)&temp[5], "Error: Server clock desynced");
  temp[4] = (TXT_TYPE_CLI_DATA << 2); // Send error as CLI_DATA
  send_ack = false; // No ACK for error
}
```

**User Experience**: Admin/user attempting to post via mesh receives error response.

### After Sync

Once `isDesynced()` returns false:
- ✅ Serial `addbulletin` command works
- ✅ Mesh client posts accepted
- ✅ All timestamps are accurate

---

## System Message Queue

### Architecture

System messages are stored separately from regular bulletin posts:
- **File**: `/system_msgs`
- **Structure**: Array of `SystemMessage` (max 8 messages)
- **Persistence**: Survives reboots
- **Per-admin delivery tracking**: Each admin receives each message exactly once

### SystemMessage Structure

```cpp
struct SystemMessage {
  char text[MAX_POST_TEXT_LEN + 1];           // Message text
  uint32_t boot_sequence;                      // Boot number (for ordering)
  uint32_t created_millis;                     // Millis since boot (for ordering within boot)
  uint8_t delivered_to[MAX_CLIENTS * 6];      // 6-byte pub_key prefixes of admins who received this
};
```

### Message Types

**1. Desync Notice** (created on boot if desynced):
```
"SYSTEM: Server rebooted. Clock desynced - read-only until admin login."
```

**2. Sync Success - Admin** (created when admin syncs via mesh):
```
"SYSTEM: Clock synced by admin [A1B2C3D4]. Server now in read-write mode."
```

**3. Sync Success - Manual** (created when clock synced via CLI):
```
"SYSTEM: Clock synced manually. Server now in read-write mode."
```

**4. Sync Success - Network** (created when repeater advertisements sync):
```
"Clock set by Repeater advert from [A1B2C3D4] to 01 Jan 2025 10:30. Quorum nodes: [A1B2C3D4], [E5F6A7B8], [C9D0E1F2]."
```

### Dual Output: Serial Console + Mesh Delivery

All system messages are output to **two destinations**:

#### 1. Serial Console (Immediate)

Messages are printed via `Serial.println()` when created:

```cpp
system_msgs->addMessage(sync_msg, current_boot_sequence);
system_msgs->save(_fs);
Serial.println(sync_msg);  // ← Immediate serial output
```

**Advantages**:
- Server admin sees notifications in real-time via USB serial monitor
- No need to wait for mesh delivery
- Useful for debugging and server monitoring

#### 2. Mesh Delivery (Queued)

Messages are pushed to admins via the regular push loop in `loop()`:

```cpp
// Check for pending system messages first (admin-only)
if (client->isAdmin()) {
  for (int i = 0; i < system_msgs->getNumMessages(); i++) {
    if (system_msgs->needsPush(i, client)) {
      // Create temporary PostInfo with timestamp=0
      PostInfo temp_post;
      temp_post.author = self_id;
      temp_post.post_timestamp = 0;  // Special marker
      strcpy(temp_post.text, sys_msg->text);

      pushPostToClient(client, temp_post);
      system_msgs->markPushed(i, client);
      system_msgs->save(_fs);
    }
  }
}
```

**Advantages**:
- Remote admins receive notifications in their companion apps
- Messages persist until delivered to all admins
- Works even if admin connects hours/days later

### Alert Queue Behavior

System messages queue like alerts - **no relevance filtering**:

```cpp
bool SystemMessageQueue::needsPush(int msg_idx, const ClientInfo* admin) {
  // Check if admin already received this message
  // ... (pub_key matching logic)
  return true;  // Always deliver if not already sent
}
```

**Result**:
- All messages delivered in order (desync, then sync)
- Admins see full alert history, not filtered by current state
- Simpler logic, scales better as more message types are added
- Example sequence:
  1. "Server rebooted. Clock desynced..."
  2. "Clock synced by admin [1234]..."

### Cleanup

Every 60 seconds, the server runs cleanup:

```cpp
system_msgs->cleanup(&acl);
```

**Logic**:
- If a message has been delivered to ALL current admin users → remove from queue
- Saves flash space and keeps queue clean
- New admins added later won't receive very old historical messages

---

## Message Ordering with Boot Counter

### The Challenge

System messages need consistent ordering across reboots, but:
- **RTC is desynced** → can't use RTC timestamps
- **Millis resets on boot** → `millis()` starts at 0 every boot
- **Flash order may vary** → file system may not preserve insertion order

### The Solution: Two-Level Timestamp

```cpp
struct SystemMessage {
  uint32_t boot_sequence;     // Which boot created this message
  uint32_t created_millis;    // Millis since that boot started
};
```

### Ordering Logic

```cpp
bool isMessageNewer(SystemMessage* a, SystemMessage* b) {
  if (a->boot_sequence != b->boot_sequence) {
    return a->boot_sequence > b->boot_sequence;  // Higher boot = newer
  }
  return a->created_millis > b->created_millis;  // Same boot, use millis
}
```

### Example

```
Boot #1 (Feb 1):
  - Message A: boot_seq=1, millis=5000

Boot #2 (Feb 2):
  - Message B: boot_seq=2, millis=3000  (lower millis, but higher boot_seq!)

Boot #3 (Feb 3):
  - Message C: boot_seq=3, millis=8000
  - Message D: boot_seq=3, millis=12000

Chronological order: A → B → C → D
```

**Boot sequence always takes precedence** over millis, ensuring correct ordering across reboots.

---

## Flash File Formats

### `/boot_count`

**Size**: 4 bytes
**Format**: Single uint32_t
**Purpose**: Tracks number of boots, incremented on each startup

```
[boot_count: 4 bytes]
```

### `/netsync_cfg`

**Size**: 8 bytes
**Format**: Network time sync configuration structure
**Purpose**: Persistent settings for repeater-based time synchronisation

```
[enabled: 1 byte]          // 0=off, 1=on (default: 0)
[maxwait_mins: 2 bytes]    // Agreement window in minutes (default: 15, range: 5-60)
[reserved: 1 byte]         // Padding for alignment
[guard: 4 bytes]           // 0xDEADBEEF validation marker
```

**Validation**: File is only accepted if `guard == 0xDEADBEEF` and `maxwait_mins` is in range [5, 60].

### `/system_msgs`

**Format**:
```
[num_messages: 1 byte]
For each message:
  [text: 152 bytes]               (MAX_POST_TEXT_LEN + 1)
  [boot_sequence: 4 bytes]        (uint32_t)
  [created_millis: 4 bytes]       (uint32_t)
  [delivered_to: MAX_CLIENTS * 6] (6-byte pub_key prefixes)
```

### `/posts`

**Regular bulletin posts** (system messages are NOT included):

```
[version: 1 byte]
[next_post_idx: 4 bytes]
For each post:
  [author_pub_key: 32 bytes]
  [post_timestamp: 4 bytes]  (RTC timestamp, must be > 0)
  [text_length: 1 byte]
  [text: variable]
```

**Note**: Posts with `timestamp == 0` are skipped during `savePosts()`.

---

## UI and Display Behaviour

### Local Display (if DISPLAY_CLASS defined)

**System messages (timestamp=0) are HIDDEN** from the local UI:

```cpp
int MyMesh::getRecentPosts(const PostInfo** dest, int max_posts) const {
  // Skip timestamp=0 entries (system messages)
  if (posts[idx].post_timestamp > 0) {
    dest[returned] = &posts[idx];
    returned++;
  }
}
```

**Reason**: System messages are for mesh admins, not for local display on the bulletin board device itself.

### Companion App (mesh clients)

**System messages ARE delivered** to admin users via push notifications:

1. System message created with `timestamp=0`
2. Pushed to admin clients via `pushPostToClient()`
3. Companion radio forwards to mobile app over BLE
4. **Mobile app displays** the system message in the bulletin board's message feed

**Result**: Admins on mobile see system messages, but the bulletin board's own display does not show them.

---

## Security Considerations

### Admin-Only Clock Sync

**Only admin users can sync the clock:**
```cpp
if (isDesynced() && !clock_synced_once && client->isAdmin()) {
  // Sync allowed
}
```

**Why**:
- Admins are authenticated via password-based login
- Admin credentials are stored in ACL with shared secrets
- Prevents guest users or attackers from setting incorrect time

### Timestamp Validation

**Minimum timestamp check**:
```cpp
if (sender_timestamp >= MIN_VALID_TIMESTAMP) {  // >= Jan 1, 2025
  // Accept timestamp
}
```

**Why**:
- Prevents acceptance of obviously wrong timestamps (year 2000, etc.)
- Ensures time only moves forward
- Admin's mobile phone syncs time from cellular network (highly accurate)

### One-Time Sync

**Clock syncs only ONCE per boot**:
```cpp
static bool clock_synced_once = false;
```

**Why**:
- Prevents time regression (clock moving backwards)
- Mitigates potential attack where compromised admin account could manipulate time
- Once synced, clock is locked for that boot session

### ACL Persistence

**Only admin users persist in ACL** (`saveFilter()`):
- Guest users are not saved to flash
- Reduces attack surface for ACL modification
- Admin list is small and controlled

### Network Time Sync Security

**Repeater-based synchronisation has additional security measures**:

**1. Cryptographic Verification**:
- All repeater advertisements are cryptographically verified by base Mesh class
- Uses Ed25519 signature validation
- Invalid signatures are rejected before timestamp is considered

**2. Quorum Requirement**:
- Requires 3 independent repeaters to agree
- Single compromised/misconfigured repeater cannot set wrong time
- Agreement window (default 15 minutes) ensures reasonable consensus

**3. Timestamp Selection**:
- Uses most recent timestamp from quorum (not median or oldest)
- Prevents accepting stale timestamps
- Ensures clock moves forward appropriately

**4. Monotonic Enforcement**:
- Selected timestamp must be > current time (if current time is valid)
- Prevents clock from moving backward
- Discards all repeaters if timestamp appears to be in the past

**5. Minimum Timestamp Validation**:
- All timestamps must be >= MIN_VALID_TIMESTAMP (Jan 1, 2025)
- Prevents acceptance of obviously wrong timestamps
- Same validation as admin sync

**6. Admin Override**:
- Admin sync always takes precedence
- Admin sync immediately clears repeater buffer
- Allows manual recovery if network sync produces wrong time

**7. One-Time Sync**:
- Same `clock_synced_once` flag as admin sync
- Prevents repeated sync attempts that could accumulate error
- Mitigates potential drift attacks

**8. Configurable/Disableable**:
- Feature can be disabled via `set nettime.enable off`
- Tolerance window is configurable (5-60 minutes)
- Admin can adjust or disable if issues arise

**Attack Resistance**:
- **Single bad actor**: Requires 3 repeaters, so 1 compromised node cannot succeed
- **Time regression**: Monotonic check and one-time sync prevent moving clock backward
- **Timestamp manipulation**: Quorum consensus and agreement window limit impact
- **Replay attacks**: Not applicable (advertisements are not replay-protected, but timestamps move forward)

---

## Edge Cases and Failure Modes

### Scenario 1: No Admin Ever Connects

**Situation**: Server boots, but no admin logs in.

**Behaviour**:
- If network time sync is enabled and repeaters are available:
  - Server automatically syncs from repeater advertisements
  - Posting enabled once 3 repeaters agree
  - System message sent to admins noting repeater sync
- If network time sync is disabled or no repeaters available:
  - Server stays in read-only mode indefinitely
  - System "desync" message remains queued
  - Clock never syncs

**Mitigation**: Network time sync now provides automatic fallback. If disabled, admin must connect after cold boot.

### Scenario 2: Admin's Clock is Wrong

**Situation**: Admin's mobile phone has incorrect time (rare, but possible).

**Behaviour**:
- Server syncs to wrong time
- All subsequent posts get incorrect timestamps
- Persists until next reboot and re-sync

**Likelihood**: Very low
- Mobile phones sync from cellular network (NTP)
- Companion radios sync from mobile phones
- Multiple layers of accuracy

**Mitigation**: Admin can manually set clock via `time <epoch>` command if needed.

### Scenario 3: Multiple Admins with Conflicting Times

**Situation**: Two admins have different times.

**Behaviour**:
- Server syncs from **first** admin to send a packet
- `clock_synced_once` flag prevents second admin from overriding

**Result**: First admin's time wins, which is acceptable.

### Scenario 4: Server Reboots Multiple Times

**Situation**: Server reboots 3 times while admin is offline.

**Behaviour**:
- 3 system "desync" messages created
- All 3 queued with different `boot_sequence` values
- Admin connects, receives all 3 messages in chronological order
- All 3 marked as delivered
- Cleanup removes them

**Impact**: Minor - admin sees multiple desync notices, but understands server rebooted multiple times.

### Scenario 5: New Admin Added Later

**Situation**: Admin B is added to ACL after system messages were created.

**Behaviour**:
- `needsPush()` checks current ACL membership
- Admin B doesn't appear in any message's `delivered_to[]` array
- Admin B receives queued messages on next login

**Result**: New admins get backlog of system messages, which provides context.

### Scenario 6: Network Sync - Only 1-2 Repeaters Heard

**Situation**: Server hears advertisements from only 1 or 2 repeaters.

**Behaviour**:
- Timestamps collected in buffer but sync does not proceed
- Requires 3 repeaters for quorum
- Buffer persists indefinitely (no timeout)
- Memory usage: 126 bytes runtime (minimal)

**Result**: Server waits for third repeater. If it never arrives, admin sync or manual CLI sync required.

### Scenario 7: Network Sync - Repeaters Disagree

**Situation**: 3 repeaters heard, but timestamps span > maxwait window (e.g., 20 minutes with 15-minute tolerance).

**Behaviour**:
- Agreement check fails
- Oldest repeater (by received_time) is discarded from buffer
- System waits for next repeater advertisement
- Process repeats until agreement found or admin intervenes

**Result**: Protects against one repeater having wrong time. Eventually finds consensus or admin syncs manually.

### Scenario 8: Network Sync - Admin Syncs During Collection

**Situation**: Server has collected 1-2 repeaters, then admin logs in and syncs.

**Behaviour**:
- Admin sync executes immediately
- Repeater buffer is cleared (`repeater_count = 0`)
- Check flag is cleared (`check_netsync_flag = false`)
- `clock_synced_once = true` prevents any future network sync attempts

**Result**: Admin sync always takes precedence, repeater collection is abandoned.

### Scenario 9: Network Sync - Timestamp Appears to be in Past

**Situation**: 3 repeaters agree, but most recent timestamp <= server's current time (and current time is valid).

**Behaviour**:
- Monotonic check fails
- All repeaters discarded (`repeater_count = 0`)
- Collection restarts from scratch

**Result**: Prevents clock from moving backward. Likely indicates repeater clocks are wrong; admin intervention recommended.

### Scenario 10: Network Sync Disabled After Partial Collection

**Situation**: Server collects 1-2 repeaters, then admin disables feature via `set nettime.enable off`.

**Behaviour**:
- `onAdvertRecv()` checks `netsync_config.enabled` on every call
- Further advertisements are ignored
- Existing buffer remains in memory (harmless)
- Admin can re-enable with `set nettime.enable on`

**Result**: Feature can be toggled at runtime without side effects.

---

## Comparison to Alternative Approaches

### ❌ NTP Sync Over WiFi
- **Requires WiFi**: Not all deployments have WiFi
- **Complexity**: Need network stack, DNS resolution
- **Security**: Opening network attack surface
- **Our approach**: Uses existing mesh infrastructure

### ❌ GPS Module
- **Cost**: Additional hardware
- **Power**: Extra power draw
- **Antenna**: Requires outdoor GPS antenna or clear sky view
- **Our approach**: Zero additional hardware

### ❌ Manual Clock Setting
- **User burden**: Admin must run command every boot
- **Error-prone**: Easy to forget or misconfigure
- **Our approach**: Fully automatic, no user intervention

### ❌ Flash-Persisted Time
- **Clock drift**: RTC drifts while powered off (no battery backup)
- **Inaccuracy accumulates**: Each boot adds more drift
- **Our approach**: Fresh sync from accurate source (cellular network)

---

## Testing and Validation

### Unit Test Scenarios

1. **Boot with desynced clock**: Verify system message created
2. **Admin login while desynced**: Verify clock syncs
3. **Second admin login**: Verify clock does NOT sync again
4. **Guest user login**: Verify clock does NOT sync
5. **Admin with invalid timestamp**: Verify clock does NOT sync
6. **System message delivery**: Verify admins receive messages
7. **Message cleanup**: Verify messages removed after delivery
8. **Flash persistence**: Verify messages survive reboot

### Integration Test Scenarios

1. **Multi-boot desync**: Reboot 3 times, verify 3 messages queued
2. **Cross-boot ordering**: Verify boot counter ensures correct order
3. **Relevance filtering**: Sync clock, verify desync messages not delivered to late-joining admin
4. **UI filtering**: Verify system messages hidden from local display
5. **Flash round-trip**: Save and load system messages, verify integrity

---

## Conclusion

The clock synchronisation system provides:

✅ **Fully automatic** time sync with zero user intervention
✅ **Multiple sync sources** - admin login, admin messages, manual CLI, and repeater advertisements
✅ **Robust fallback** - network time sync from repeater infrastructure when no admin available
✅ **Quorum consensus** - requires 3 repeaters to agree, preventing single point of failure
✅ **Secure** - admin-only direct sync, cryptographically verified repeater adverts, configurable tolerance
✅ **Reliable** - one-time sync prevents time regression, monotonic enforcement
✅ **Informative** - system messages keep admins aware of server state and sync source
✅ **Persistent** - system messages survive reboots and track delivery
✅ **Clean** - automatic cleanup prevents message accumulation
✅ **Configurable** - network sync can be enabled/disabled and tuned via CLI commands

This architecture ensures the bulletin board server maintains accurate time for post ordering and synchronisation, with multiple sync pathways for maximum reliability, while handling edge cases gracefully and keeping admins informed.
