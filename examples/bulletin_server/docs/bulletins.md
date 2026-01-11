# Bulletins and Posts

## Overview
The bulletin server stores text posts from mesh users and server-generated bulletins. The server assigns timestamps using its own RTC and synchronizes posts to connected clients.

## Post types
- User posts: plain text from READ_WRITE or ADMIN users.
- Bulletins: server-generated posts with severity (info, warning, critical) and a `BLTN-` prefix.

## Creation paths
- Mesh user messages (TXT_TYPE_PLAIN).
- Admin or serial CLI: `bulletin.info|warning|critical <text>`.
- UI long press: posts a WARNING alarm bulletin.

## Delivery and sync
- Posts are stored in a circular buffer (max 32) and persisted to `/posts`.
- The server pushes posts to active clients in round-robin order and waits for ACKs.
- Posts are not pushed back to their author.
- WARNING and CRITICAL bulletins are also broadcast to the group channel. See broadcast-channel.md.

## Limits and restrictions
- Max message length is 140 characters (before severity prefix).
- Bulletins are rate-limited to one every 10 seconds.
- Posting is blocked when the server clock is desynced (mesh and serial paths). See timesync.md.

## Troubleshooting
- If posts are rejected, confirm the user role and clock sync status.
- If posts are not delivered, ensure clients are logged in and not hitting ACK timeouts.
