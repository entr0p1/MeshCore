# Broadcast Channel

## Overview
Warning and critical bulletins are duplicated to a MeshCore group channel so nodes can receive urgent alerts even without a room session. Info bulletins are local only.

## Channel modes
- Public (default): channel secret is derived from the server public key (first 16 bytes).
- Private: channel secret is a random 16-byte key stored in flash; switching to private regenerates the key.

## How broadcasts are sent
- Broadcasts are best-effort group messages (no ACKs).
- Message text is formatted as `<node_name>: BLTN-WARN: <text>` or `<node_name>: BLTN-CRIT: <text>`.
- Broadcasts occur in addition to normal post storage and sync.

## Admin controls
- `get channel.mode` shows current mode.
- `set channel.mode public` switches to public mode.
- `set channel.mode private` switches to private mode and generates a new secret.
- `channel`, `channel public`, and `channel private` are shorthand equivalents.
- Users can retrieve the current key with `!channelkey`.

## Storage
- `/channel_cfg` stores the mode and secret.

## Troubleshooting
- If nodes do not receive broadcasts, confirm they are subscribed to the correct group channel and that the key matches `!channelkey`.
- Very long bulletins may be truncated by group payload limits; keep alerts concise.
