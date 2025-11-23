# File Persistence

The bulletin board saves all critical data to flash storage to survive reboots.

## Posts Storage

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

## System Messages Storage

System messages are stored separately in `/system_msgs`:
```
[num_messages:1][message_1][message_2]...[message_8]
```

Each system message contains:
- Text (152 bytes)
- Boot sequence (4 bytes)
- Created millis (4 bytes)
- Delivered-to tracking (MAX_CLIENTS * 6 bytes)

## Boot Counter

Boot sequence tracking stored in `/boot_count`:
- Single uint32_t value
- Incremented on each boot
- Used for system message ordering

## Platform Compatibility

The persistence implementation uses platform-specific file operations to ensure compatibility across:
- **NRF52**: InternalFileSystem
- **RP2040**: LittleFS
- **STM32**: LittleFS
- **ESP32**: SPIFFS

All platforms use a common abstraction layer for file I/O.
