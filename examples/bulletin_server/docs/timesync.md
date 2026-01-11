# Clock Synchronization

## Overview
The bulletin server timestamps posts using its RTC. If the RTC is earlier than 2025-01-01 00:00:00 UTC, the server treats the clock as desynced and enters read-only mode for posting. Existing posts can still be read and synced to clients. Admins receive system messages when the server boots desynced and when it syncs.

## Sync sources
The server can sync from the same sources as MeshCore plus optional repeater sync:
- Admin auto-sync: first admin login or admin message after boot, using the sender timestamp.
- Manual CLI sync: `time <epoch>` or `clock sync` (CommonCLI).
- Network time sync (optional): repeater adverts, enabled with `set nettime.enable on`.

Auto-sync happens once per boot; subsequent admin packets are ignored for sync.

## Network time sync behavior
When enabled, the server waits for three unique repeater adverts with timestamps that agree within a configurable window:
- Agreement window: `nettime.maxwait` minutes (5-60, default 15).
- Sync timestamp: most recent of the three.
- Monotonic guard: the server will not move the clock backwards.
- Runs only while desynced and only once per boot.

## Troubleshooting
- Server stays read-only: confirm the admin device clock is correct (>= 2025-01-01) and send any admin packet, or set the time manually.
- Network sync not happening: ensure `nettime.enable` is on, check `get nettime.status`, and verify three repeater adverts within the `nettime.maxwait` window.

## Related documentation
- system-messages.md
