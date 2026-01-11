# On-Device UI

## Overview
When built with DISPLAY_CLASS, the bulletin server provides a simple on-device UI. The UI pulls posts from the mesh layer; it does not receive pushed data.

## Screens
- Splash: firmware version and build date.
- Status: node name, ACL counts, SD card usage (used/total), battery indicator, and clock status.
- Radio config: frequency, spreading factor, bandwidth, coding rate, TX power, and noise floor.
- Messages: up to three most recent posts with relative age; warning and critical bulletins are color-coded. If the clock is desynced, the age shows `NOSYNC`.

## Controls
- Short press: cycle screens (Status -> Radio -> Messages -> Status).
- Long press: post an alarm bulletin (`ALARM at HH:MM - DD/MM/YYYY UTC`).
- Auto-off: display turns off after 15 seconds of inactivity; any button press wakes it.

## Notes
- SD usage shows `used/total` or a status string such as `no card` or `Not supported`.
- System messages (timestamp 0) are not shown in the on-device message list.
- Alarm bulletins use the server RTC; if the clock is wrong, the timestamp will be wrong.
