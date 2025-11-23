# Broadcast Channel

The bulletin board supports broadcasting WARNING and CRITICAL bulletins to all nodes in the mesh via a shared channel. This allows important alerts to reach everyone, not just connected clients.

## Channel Modes

### Public Mode (default)
- Uses the server's public key as the channel key
- Anyone can listen to broadcasts by knowing the server's identity
- Suitable for open networks where all users trust each other

### Private Mode
- Uses a randomly generated 16-byte secret key
- Only users with the channel key can decrypt broadcasts
- Key is shared via the `channel private` command output or `!channelkey` user command
- Suitable for private networks with access control

## Channel Configuration

**View current mode:**
```
channel
```

**Switch to public mode:**
```
channel public
```

**Switch to private mode:**
```
channel private
```
Output:
```json
{"component":"config","action":"update","data":{"type":"channel","mode":"private","key":"A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6"}}
```

**Get channel key (user command):**
```
!channelkey
```
Returns the 32-character hex key (16 bytes) for the current channel.

## Broadcast Behaviour

- Only WARNING and CRITICAL severity bulletins are broadcast
- Broadcasts use `PAYLOAD_TYPE_GRP_TXT` group message format with `TXT_TYPE_SIGNED_PLAIN`
- Server identity (pub_key + name) included in all broadcasts for authenticity
- All nodes on the same channel receive broadcasts immediately
- No ACKs or delivery confirmation (fire-and-forget)
- Compatible with MeshCore group channel functionality
- Messages display identically in channel and room views

## Configuration Storage

Channel configuration is persisted to flash at `/channel_cfg`:
```cpp
struct BulletinChannelConfig {
  bool mode_private;         // false=public, true=private
  uint8_t secret[16];        // Private channel key (only used if mode_private==true)
  uint32_t guard;            // 0xDEADBEEF validation marker
};
```
