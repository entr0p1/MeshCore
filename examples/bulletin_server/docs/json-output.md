# JSON Serial Output

## Overview
The bulletin server prints line-delimited JSON records to the serial console for posts, channel events, user command audits, and external app requests/responses. Non-JSON lines can also appear (CLI echo, debug output).

## Common structure
Each JSON line uses:
- component: subsystem name
- action: event type
- data: event fields
- meta: timestamp and source; user fields when available

`data.type` is always present for event logs. `meta.user_pubkey` and `meta.user_role` only appear when a user is involved.

## Event logs

### post / create
- data.type: `message` or `bulletin`
- data.text: message content (bulletins use the raw text without the `BLTN-...` prefix)
- data.severity: `info`, `warning`, `critical` (bulletins only)
- meta.source: `mesh` for user posts, `console` for server bulletins

Example:
`{"component":"post","action":"create","data":{"type":"bulletin","severity":"warning","text":"Severe weather"},"meta":{"source":"console","timestamp":1735689600}}`

### channel / config
- data.type: `mode`
- Public mode output uses `data.text` set to `public` (legacy format).
- Private mode output uses `data.mode` set to `private` and includes `data.secret`.

Example (private):
`{"component":"channel","action":"config","data":{"type":"mode","mode":"private","secret":"<hex>"},"meta":{"source":"console","timestamp":1735689600}}`

### channel / broadcast
- data.type: `bulletin`
- data.severity: `warning` or `critical`
- data.text: bulletin text (without prefix)

### app / request or response (command audit)
- data.type: `command`
- data.text: the user command or the server reply
- meta.user_pubkey and meta.user_role are included
- Valid `!app` requests emit a request audit and a separate RPC request, but no audit response.

## External app RPC messages

### app / request
Printed when a user issues `!app`:
- data.app_name
- data.command
- meta.user_pubkey, meta.source = `mesh`, meta.timestamp

### app / response
Printed when `appreply` succeeds:
- data.type: `data`
- data.app_name
- data.text
- meta.user_pubkey, meta.source = `console`, meta.timestamp

## Parsing notes
- One JSON object per line.
- `text` fields are not JSON-escaped; quotes in text will break JSON.
- Expect non-JSON lines (CLI echo, debug, system message logs).
- Timestamps are 32-bit Unix seconds.
