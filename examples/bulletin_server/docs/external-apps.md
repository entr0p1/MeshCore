# External Application Integration

## Overview
The bulletin server can forward user requests to an external process over the serial console and relay responses back to the user. Requests are issued with the `!app` user command.

## Request flow
1. A READ_WRITE or ADMIN user sends `!app <app_name> <command>`.
2. The server replies to the user with `Processing request...` and prints a JSON request to serial.
3. The external app processes the request and responds with `appreply ...`.
4. The server delivers the response as a signed message from the server.

## JSON request format (serial output)
Single-line JSON:
`{"component":"app","action":"request","data":{"app_name":"<name>","command":"<command>"},"meta":{"user_pubkey":"<64-hex>","source":"mesh","timestamp":<unix>}}`

## Response command
`appreply <app_name> <user_pubkey_hex> <response_text>`

Notes:
- `app_name` must match the request.
- `user_pubkey_hex` must be the 64-character hex value from the request.
- `response_text` is sent verbatim to the user.
- The server also emits a JSON response line when `appreply` succeeds.

## Limits and timeouts
- One pending request per user at a time.
- If no response is received within 10 seconds, the server sends `Request timeout - no response from app`.

## Security and robustness
- Validate and sanitize inputs; do not execute shell commands from user input.
- Use allowlists for commands and check user pubkeys for authorization.
- Keep responses concise to avoid packet truncation.

## Troubleshooting
- Timeouts usually indicate no `appreply` within 10 seconds or a mismatched pubkey.
- `ERROR: Client not found` means the target user is not currently connected.
