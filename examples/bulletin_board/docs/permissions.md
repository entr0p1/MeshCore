# Permissions & Access Control

The bulletin board implements a three-tier permission system to control what users can do.

## Permission Levels

### **GUEST (0)** - Read-Only Access

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

### **READ_WRITE (2)** - Standard User

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

### **ADMIN (3)** - Full Privileges

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

## Permission Comparison Table

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

## Password Configuration

**Set admin password** (via serial CLI):
```
password <new_password>
```

**Set guest/room password** (default: "hello"):
```
set guest_password <password>
```

**Enable/disable guest read-only access:**
```
set allow_read_only on   # Allow passwordless guest access
set allow_read_only off  # Require guest password or ACL entry
```

---

## Login Flow

When a client connects, the server checks credentials in this order:

1. **Admin password match** → Grant `ADMIN` permissions
2. **Guest password match** → Grant `READ_WRITE` permissions
3. **No password + allow_read_only=true** → Grant `GUEST` permissions
4. **No password + allow_read_only=false** → Check ACL, reject if not found
5. **Wrong password** → Reject connection (no response, client times out)

---

## Access Control List (ACL)

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

## Clock-Based Restrictions

The bulletin board enforces **read-only mode** when the server's RTC is desynced (year < 2025):

**Blocked operations:**
- ❌ `addbulletin` command (serial) - returns error
- ❌ Post creation (READ_WRITE and ADMIN) - returns error message
- ❌ Alarm button posts (long press) - silently blocked (TODO: Add error message to UI)

## Emergency Alarm Feature

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

## Differences from simple_room_server

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
