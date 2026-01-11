# File Persistence

## Overview
The bulletin server stores state on the platform filesystem so it survives reboot. Files are stored on the on-device flash filesystem unless otherwise noted.

## Persisted files
- `/com_prefs`: MeshCore preferences (node name, passwords, radio settings, and other CommonCLI values).
- `/s_contacts`: admin access list (admins only).
- `/posts`: circular buffer of up to 32 posts (author pubkey, timestamp, text). System messages (timestamp 0) are not saved here.
- `/system_msgs`: queue of up to 8 system messages with per-admin delivery tracking.
- `/boot_count`: 32-bit boot sequence used in system message formatting.
- `/channel_cfg`: broadcast channel mode and secret.
- `/netsync_cfg`: network time sync settings.
- `/packet_log`: packet log when logging is enabled.

## SD card backups (ESP32 only)
When an SD card is present, the server mounts a `/bulletin` directory. Configuration files are mirrored to SD on change so a second copy is always available:
- `/com_prefs`
- `/s_contacts`
- `/channel_cfg`
- `/netsync_cfg`

On boot, if a flash config file is missing or unreadable, it is restored from the SD copy. After `erase.sdcard`, the current config is copied back to the SD card.

## Platform filesystems
- NRF52: InternalFileSystem
- RP2040: LittleFS
- STM32: LittleFS
- ESP32: SPIFFS
