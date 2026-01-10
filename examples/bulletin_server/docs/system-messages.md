# System Messages Architecture

## Overview

System messages are special notifications sent to admin users over the mesh about server state changes. For example:

1. **Clock desync on boot** - Server is in read-only mode
2. **Clock sync success** - Server transitions to read-write mode
3. **Future state changes** - Architecture allows for additional message types

---

## Critical Design Constraints

### 1. Companion App Deduplication

**Problem**: The companion app deduplicates messages based on `(timestamp, text)` pairs.

**Why it matters**:
- If two messages have identical timestamp AND text, the app silently discards the duplicate
- This is intentional - prevents showing "test test test" 20 times unnecessarily
- BUT causes issues for system messages

**Example of the problem**:
```
Boot #1:
  - RTC defaults to 1715770351 (May 2024)
  - Create: "SYSTEM: Server rebooted. Clock desynced..."
  - Tuple: (1715770351, "SYSTEM: Server rebooted...")

Boot #2:
  - RTC defaults to 1715770351 (SAME!)
  - Create: "SYSTEM: Server rebooted. Clock desynced..." (SAME TEXT!)
  - Tuple: (1715770351, "SYSTEM: Server rebooted...") (IDENTICAL!)
  - App says: "I already have this" → SILENT DISCARD
```

**Solution**: Include boot sequence number in the message text itself:
```
"SYSTEM: boot:1 msg:Server rebooted. Clock desynced..."
"SYSTEM: boot:2 msg:Server rebooted. Clock desynced..."
```

Now each boot's message has **different text**, preventing deduplication even if timestamps collide.

### 2. ACK-Based Delivery Tracking

**Problem**: System messages must be marked as "delivered" only AFTER the client acknowledges receipt.

**Why it matters**:
- LoRa has packet loss (~5-10% typical)
- If message is marked delivered before ACK, packet loss means message is lost forever
- Server won't retry because it thinks client already received it

**Solution**: Track pending system message index per client, mark delivered in `processAck()` after ACK confirmed.

**Implementation**:
```cpp
// On push:
pushPostToClient(client, temp_post);
pending_system_msg_idx[next_client_idx] = i;  // Track for ACK

// On ACK receipt:
if (pending_system_msg_idx[i] >= 0) {
  system_msgs->markPushed(pending_system_msg_idx[i], client);
  pending_system_msg_idx[i] = -1;
}

// On ACK timeout:
pending_system_msg_idx[i] = -1;  // Allow retry
```

### 3. Timestamp = 0 Marker

**Problem**: System messages need to be distinguished from regular posts.

**Why timestamp=0**:
- Regular posts always have timestamps > MIN_VALID_TIMESTAMP (Jan 1, 2025)
- timestamp=0 is a clear, unambiguous marker
- Used to filter system messages from local UI display (admins see them via push, local display doesn't)

**Important**: The companion app CAN display timestamp=0 messages (shows as "Jan 1 1970"). The boot number in the text is what prevents deduplication, NOT the timestamp.

### 4. Pre-Login Delivery

**Problem**: System messages need to reach admins BEFORE they log in.

**Why it matters**:
- The "Clock desynced - read-only until admin login" message is pointless if it only arrives AFTER login
- Regular posts only go to active (logged-in) clients
- Admins need notification of server state before connecting

**Solution**: Allow up to 3 pre-login delivery attempts per admin per system message.

**Implementation**:
```cpp
uint8_t system_msg_prelogin_attempts[MAX_CLIENTS][8];  // Track attempts per client per message

// In push loop:
if (!is_active && system_msg_prelogin_attempts[client_idx][msg_idx] >= 3) {
  continue;  // Skip, exhausted attempts
}

if (!is_active) {
  system_msg_prelogin_attempts[client_idx][msg_idx]++;
  // Push system message even though client hasn't logged in
}

// On successful ACK:
system_msg_prelogin_attempts[client_idx][msg_idx] = 0;  // Reset counter

// On login:
memset(system_msg_prelogin_attempts[client_idx], 0, ...);  // Reset all counters
```

**Why 3 attempts**:
- Gives reasonable chance of delivery despite LoRa packet loss (~5-10%)
- Limits airtime usage (3 attempts × ~100 bytes × number of admins)
- After 3 failed attempts, message delivery waits until admin logs in
- Once logged in, normal delivery tracking takes over (no limit on retries)

**Important**: Pre-login attempts are per-admin per-message. Each admin gets 3 attempts for each system message independently.

---

## Creating System Messages

### ALWAYS Use the Centralised Function

```cpp
void MyMesh::addSystemMessage(const char* message);
```

**Format**: `"SYSTEM: boot:[n] msg:[your message]"`

### DO NOT Create Messages Manually

❌ **WRONG** - Bypasses boot number formatting:
```cpp
system_msgs->addMessage("SYSTEM: Something happened", current_boot_sequence);
```

✅ **CORRECT** - Uses centralised function:
```cpp
addSystemMessage("Something happened");
```

### Why Centralisation Matters

1. **Guarantees uniqueness** - Boot number automatically included
2. **Consistent formatting** - All system messages look the same
3. **Future-proof** - If format needs to change, update one place
4. **Prevents bugs** - Developers can't accidentally create duplicate-prone messages

---

## Message Flow

### 1. Message Creation
```
addSystemMessage("Server rebooted...")
  ↓
Format: "SYSTEM: boot:5 msg:Server rebooted..."
  ↓
Store in SystemMessageQueue with boot_sequence=5
  ↓
Save to flash (/system_msgs)
  ↓
Print to serial console
```

### 2. Message Delivery (Per Admin)
```
loop() - Round-robin through clients
  ↓
Check: client->isAdmin()?
  ↓
Check: is_active = (client->last_activity != 0)?
  ↓
For each system message:
  ↓
  Check pre-login attempts (if !is_active):
    system_msg_prelogin_attempts[client_idx][msg_idx] >= 3?
      YES: Skip this message (exhausted attempts)
      NO: Continue
  ↓
  needsPush(msg_idx, client)?
    - Not already delivered to this admin?
    - Returns true if admin hasn't received it
  ↓
  pushPostToClient(client, temp_post with timestamp=0)
  ↓
  pending_system_msg_idx[client_idx] = msg_idx
  ↓
  Increment pre-login counter (if !is_active):
    system_msg_prelogin_attempts[client_idx][msg_idx]++
  ↓
  Wait for ACK (up to 3 retries on timeout)
  ↓
  On ACK:
    markPushed(msg_idx, client)
    system_msg_prelogin_attempts[client_idx][msg_idx] = 0  // Reset counter
  ↓
  Save delivery tracking to flash

On admin login:
  Reset all pre-login attempts: memset(system_msg_prelogin_attempts[client_idx], 0, ...)
  Normal delivery tracking takes over
```

### 3. Cleanup
```
Every 60 seconds:
  ↓
For each system message:
  Has it been delivered to ALL current admins?
  ↓
  YES: Remove from queue
  NO: Keep for future delivery
```

---

## File Persistence

### Boot Counter (`/boot_count`)
- Single `uint32_t` value
- Incremented on every boot
- Provides total ordering across reboots

### System Messages (`/system_msgs`)
```
[num_messages:1 byte]
[message_0]
[message_1]
...
[message_N]
```

Each message contains:
```cpp
struct SystemMessage {
  char text[152 bytes];                  // Formatted with boot number
  uint32_t boot_sequence;                // Boot number when created
  uint32_t created_millis;               // millis() when created
  uint8_t delivered_to[MAX_CLIENTS * 6]; // 6-byte pub_key prefixes
};
```

---

## Common Pitfalls

### ❌ DON'T: Remove Boot Number to "Clean Up" Format

**Why**: You'll reintroduce the deduplication bug. Every reboot will create messages with identical text, causing silent discards in the companion app.

### ❌ DON'T: Use Non-Zero Timestamps

**Why**: System messages are filtered from local UI display using `timestamp=0` marker. Non-zero timestamps would show them on the bulletin server display.

### ❌ DON'T: Mark Delivered Before ACK

**Why**: Packet loss means message is lost forever. Always wait for ACK confirmation.

### ❌ DON'T: Create Messages Directly

**Why**: Bypasses boot number formatting, causing deduplication issues.

---

## Testing System Messages

### Test Scenario 1: Cold Boot
```
1. Unplug USB (full power cycle)
2. Wait 10 seconds
3. Plug in USB
4. Check serial: "SYSTEM: boot:X msg:Server rebooted..."
5. Login as admin
6. Check serial: "SYSTEM: boot:X msg:Clock synced..."
7. Check companion app: Should see BOTH messages
```

### Test Scenario 2: Multiple Reboots
```
1. Cold boot #1 → Login → Check app (boot:1 messages)
2. Cold boot #2 → Login → Check app (boot:2 messages)
3. Cold boot #3 → Login → Check app (boot:3 messages)
4. Verify: Each boot's messages are separate (not deduplicated)
```

### Test Scenario 3: Packet Loss Retry
```
1. Cold boot
2. Login as admin
3. Monitor debug output for:
   - "pushed system message 0 to admin XX, awaiting ACK"
   - "System message 0 ACK timeout for client XX, will retry"
   - "System message 0 ACKed by admin XX, marked delivered"
4. Verify: Retries happen, delivery confirmed
```

---

## Serial Output

System messages produce two levels of Serial output:

### Production Output (Always On)

High-level status messages that operators need to see, printed regardless of debug mode. All messages are prefixed with the module name to distinguish console logs from actual mesh-delivered content.

**Message creation (actual mesh-delivered message):**
```
SystemMessageQueue: Message 0 queued: SYSTEM: boot:5 msg:Server rebooted. Clock desynced - read-only until admin login.
```

**User login (all users):**
```
MyMesh: User login: [A1B2C3D4] (admin)
MyMesh: User login: [12345678] (user)
```

**Pre-login delivery attempts:**
```
SystemMessageQueue: Message 0 delivery attempt 1/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 delivery attempt 2/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 delivery attempt 3/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 pre-login attempts exhausted for admin [A1B2C3D4] - queued until login
```

**Delivery attempts for logged-in admins (with retry tracking):**
```
SystemMessageQueue: Message 1 delivery attempt 1/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 1 delivered to admin [A1B2C3D4]
```

**If delivery fails 3 times (for any admin):**
```
SystemMessageQueue: Message 0 delivery attempt 1/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 delivery attempt 2/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 delivery attempt 3/3 to admin [A1B2C3D4]
SystemMessageQueue: Message 0 attempts exhausted for admin [A1B2C3D4] - queued until next login
```

**Successful delivery:**
```
SystemMessageQueue: Message 0 delivered to admin [A1B2C3D4]
```

**Notes:**
- **Module prefixes distinguish message types:**
  - `SystemMessageQueue:` - Messages about system message queue operations
  - `MyMesh:` - Messages about mesh operations (login, etc.)
- **"Message X queued:"** shows message creation with ID assignment
- **"SYSTEM:" prefix** in the text indicates actual mesh-delivered message content
- **Message IDs** (0, 1, 2, etc.) track individual messages through the delivery lifecycle
- **Consistent ID format:** All admin/user IDs use 8-character hex: `[%02X%02X%02X%02X]` (first 4 bytes of pub_key)
- **Retry behaviour:** System messages are retried up to 3 times for ALL admins (both pre-login and post-login)
  - After 3 failed attempts, the message is queued until the admin logs in again
  - When admin logs in, attempt counters are reset to 0 and delivery is retried
- **Easy filtering:** Use module prefixes to filter console output by subsystem
- **Boot delay:** 5-second delay after boot ensures Serial console is initialised before system messages are created

### Debug Output (MESH_DEBUG=1)

Detailed technical traces for troubleshooting, only visible when `MESH_DEBUG=1` is defined:

**On Boot:**
```
DEBUG: Loaded X system messages from flash
DEBUG: RTC current_time=..., MIN_VALID=1735689600, isDesynced=1
DEBUG: Added system message (boot 5), now have 1 messages
```

**On Push:**
```
DEBUG: loop - checking for client 18, isAdmin=1, is_active=0, num_sys_msgs=2
DEBUG:   sys_msg[0]: needsPush=1, prelogin_attempts=0
DEBUG:     needsPush[0]: checking msg='SYSTEM: boot:5 msg:Server...'
DEBUG:     needsPush[0]: YES, needs push to 18300741
DEBUG: loop - pushed system message 0 to INACTIVE admin 18 (pre-login attempt 1/3), awaiting ACK
```

**On ACK:**
```
DEBUG: System message 0 ACKed by admin 18, marked delivered
```

**On Cleanup:**
```
DEBUG: System message cleanup: removed 2 messages
```

---

## Adding New System Message Types

When adding new system messages in the future:

1. **Identify the trigger event** (e.g., "Low battery", "Firmware update available")
2. **Use the centralised function**:
   ```cpp
   addSystemMessage("Low battery: 15% remaining");
   ```
3. **Don't worry about**:
   - Boot number formatting (automatic)
   - Deduplication (handled by boot number)
   - ACK tracking (framework handles it)
   - Cleanup (automatic after delivery)

---

## References

- **System Message Queue**: `SystemMessageQueue.h/cpp` - Persistence and delivery tracking
- **Clock Sync Logic**: `timesync-logic.md` - Complete technical details
- **Main Implementation**: `MyMesh.cpp` - Search for `addSystemMessage()`
- **Companion App**: Messages displayed with timestamp from local device, ordered by receipt time

---

## Revision History

- **2025-10-27**: Restructured Serial output with module prefixes
  - Added module prefixes (`SystemMessageQueue:`, `MyMesh:`) to distinguish console logs from mesh content
  - Added message IDs (0, 1, 2, etc.) to all delivery tracking messages for lifecycle visibility
  - Added "Message X queued:" format to message creation output for clear ID assignment
  - Added 5-second boot delay to ensure Serial console initialisation before creating system messages
  - **Unified retry tracking:** System messages now retry up to 3 times for ALL admins (both pre-login and post-login)
    - Attempts are tracked consistently using the same counter for both scenarios
    - After 3 failed attempts, messages are queued until admin logs in again
    - Login resets attempt counters and allows redelivery
  - Removed premature "Attempting redelivery" message (delivery attempts are logged as they occur)
  - Added delivery attempt logging for logged-in admins (not just pre-login)
  - Moved message creation Serial output to SystemMessageQueue class
  - User login messages for all users (not just admins)
  - Standardised admin/user ID format to `[%02X%02X%02X%02X]` (8 hex chars) across all Serial output
  - Consistent formatting with "Clock synced by admin [XXXXXXXX]" messages

- **2025-10-26**: Initial documentation
  - Centralised message creation
  - Boot number formatting to prevent deduplication
  - ACK-based delivery tracking
  - Comprehensive testing scenarios
