# Permissions and Access Control

## Overview
The bulletin server uses the MeshCore room permission model with three roles. Only admins are persisted to the ACL.

## Roles

### Guest (0)
How granted: no password and `allow_read_only` enabled.

Capabilities:
- View posts.
- Cannot post, use user commands, or run CLI commands.
- Not saved to ACL.

### Read_write (2)
How granted: guest/room password.

Capabilities:
- View and create posts when the clock is synced.
- Use user commands (`!help`, `!version`, and others).
- Cannot run CLI commands or receive system messages.
- Not saved to ACL.

### Admin (3)
How granted: admin password or ACL entry.

Capabilities:
- Full CLI access over mesh or serial.
- Automatic clock sync on first valid admin packet after boot.
- Receive system messages.
- Full telemetry access.
- Saved to ACL.

## Login flow
1. Admin password -> admin.
2. Guest/room password -> read_write.
3. No password + `allow_read_only` -> guest.
4. No password + `allow_read_only` off -> ACL check.
5. Wrong password -> no response (client times out).

## Password configuration (CommonCLI)
- `password <new_password>`
- `set guest_password <password>`
- `set allow_read_only on|off`

## ACL management
- `get acl` prints current admin list (serial).
- `setperm <pubkey_hex> <permissions>` updates permissions; admin entries persist.
- ACL is stored in `/s_contacts`.

## Clock-based restrictions
- When the clock is desynced, mesh and serial posts are rejected.
- Admin CLI still works; use manual time sync if needed. See timesync.md.

## Emergency alarm (UI)
- Long press posts a WARNING bulletin with server identity.
- Delivered like a normal bulletin; not a system message.
- Requires an active client session for immediate delivery; otherwise stored and delivered on next login.
- Uses the server RTC; if the clock is desynced, the timestamp may be wrong.
