# System Messages

## Overview
System messages are admin-only notifications about server state changes (clock desync/sync, channel mode changes, network time sync). They are delivered over the mesh and logged to serial.

## Format and deduplication
- Text format: `SYSTEM: boot:<n> msg:<text>`.
- The boot number prevents companion app deduplication when the RTC resets and identical messages are created.
- Messages are delivered with timestamp 0 to keep them distinct from normal posts.

## Queue and persistence
- Stored in `/system_msgs` with a maximum of 8 messages.
- Per-admin delivery tracking is persisted so each admin receives each message once.
- The boot sequence counter is stored in `/boot_count`.
- System messages are not stored in `/posts` and are not shown in the on-device message list.

## Delivery behavior
- Admin-only; checked before regular post delivery.
- Up to 3 delivery attempts per admin per message, including pre-login attempts.
- Messages are marked delivered only after an ACK.
- After 3 failed attempts, delivery waits until the admin logs in again.
- A periodic cleanup removes messages delivered to all current admins.

## Operational signals
Serial logs include message creation and delivery attempts with the prefixes `SystemMessageHandler:` and `SystemMessageQueue:`.

## Related documentation
- timesync.md
