# Command Reference

## User commands (mesh messages starting with `!`)
Available to READ_WRITE and ADMIN users.

- `!help` or `!help <cmd>`: list commands or show help.
- `!version`: firmware version, build date, MeshCore version, and role.
- `!channel`: show current broadcast channel mode.
- `!channelkey`: show current channel key (public or private).
- `!rxp`: show the receive path used for the last message (FLOOD or DIRECT).
- `!txp`: show the server transmit path to the client (DIRECT path if known).
- `!app <app_name> <command>`: send a request to an external app (see external-apps.md).

Responses are delivered as signed messages from the server and are only visible to the requesting user.

## Admin and serial CLI (bulletin server)
These commands can be run on the serial console, or over mesh by an admin using CLI packets.

- `bulletin.info <text>`
- `bulletin.warning <text>`
- `bulletin.critical <text>`
- `get channel.mode`
- `set channel.mode public|private`
- `channel`, `channel public`, `channel private` (shorthand)
- `setperm <pubkey_hex> <permissions>`
- `get acl`
- `login.history` (last 5 logins, in-memory only)
- `appreply <app_name> <pubkey_hex> <response_text>`
- `set nettime.enable on|off`
- `get nettime.enable`
- `set nettime.maxwait <5-60>`
- `get nettime.maxwait`
- `get nettime.status`
- `erase.sdcard` (ESP32 with SD only)

## CommonCLI (MeshCore)
All standard MeshCore CommonCLI commands are available (for example: `password`, `set guest_password`, `set allow_read_only`, `time`, `clock sync`, and `get`/`set` preferences). Refer to the MeshCore documentation for full syntax.
