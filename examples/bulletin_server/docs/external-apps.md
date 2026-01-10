# External Application Integration

The bulletin server provides a bidirectional command/response system that allows external applications (running on the same host via serial) to receive commands from mesh users and send responses back.

## Overview

Users send commands to external applications using the `!app` user command. The server forwards these requests via JSON to the serial console, where external applications can process them and respond using the `appreply` CLI command. JSON reply capability coming soon.

## User Workflow

### Sending Commands

Connected clients can send commands to external applications:

```
!app weather forecast
!app sensor temperature
```

**Format:** `!app <app_name> <command>`

**Response:**
1. Immediate acknowledgement: `"Processing request..."`
2. Application response (within 10 seconds) or timeout message

### Timeout Behaviour

If the external application doesn't respond within **10 seconds**:
- User receives: `"Request timeout - no response from app"`
- Pending request flag is cleared
- User can send a new request

## Application Integration

### Receiving Requests

When a user sends `!app weather forecast`, the server outputs this JSON to serial:

```json
{
  "component": "app",
  "action": "request",
  "data": {
    "app_name": "weather",
    "command": "forecast"
  },
  "meta": {
    "user_pubkey": "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6A7B8C9D0E1F2G3H4I5J6K7L8M9N0O5P6",
    "source": "mesh",
    "timestamp": 1735689600
  }
}
```

**Fields:**
- `app_name` - Target application identifier
- `command` - Command string to execute
- `user_pubkey` - 64-character hex string (32 bytes) of requesting user
- `source` - Always "mesh" for user commands
- `timestamp` - Unix timestamp of the request

### Sending Responses

Applications respond using the `appreply` CLI command via serial:

```
appreply weather A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6A7B8C9D0E1F2G3H4I5J6K7L8M9N0O5P6 Sunny, 22°C
```

**Format:** `appreply <app_name> <user_pubkey_hex> <response_text>`

**Parameters:**
- `app_name` - Must match the app_name from the request
- `user_pubkey_hex` - 64-character hex string from the request's `user_pubkey` field
- `response_text` - Free-form text response (remainder of line)

**Output:**
```json
{
  "component": "app",
  "action": "response",
  "data": {                                   
    "type": "data",
    "app_name": "weather",
    "text": "Sunny, 22°C"
  },
  "meta": {
    "user_pubkey": "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6A7B8C9D0E1F2G3H4I5J6K7L8M9N0O5P6",
    "source": "console",
    "timestamp": 1735689605
  }
}
```

The response is delivered to the user as a signed message from the server.

## Example Application (Python)

```python
#!/usr/bin/env python3
import json
import sys
import serial

def handle_weather_request(command, user_pubkey):
    """Handle weather application requests"""
    if command == "forecast":
        return "Sunny, 22°C with light winds"
    elif command == "alerts":
        return "No weather alerts"
    else:
        return f"Unknown weather command: {command}"

def process_json_line(line, ser):
    """Process a single JSON line from serial"""
    try:
        data = json.loads(line)

        # Check if this is an app request
        if data.get("component") == "app" and data.get("action") == "request":
            app_name = data["data"]["app_name"]
            command = data["data"]["command"]
            user_pubkey = data["meta"]["user_pubkey"]

            # Route to appropriate handler
            if app_name == "weather":
                response = handle_weather_request(command, user_pubkey)
                # Send response back via serial
                reply = f"appreply {app_name} {user_pubkey} {response}\n"
                ser.write(reply.encode())

    except json.JSONDecodeError:
        pass  # Not JSON, ignore
    except KeyError as e:
        print(f"Missing field in JSON: {e}", file=sys.stderr)

def main():
    # Open serial connection
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

    print("External app handler started. Listening for requests...", file=sys.stderr)

    buffer = ""
    while True:
        # Read incoming data
        if ser.in_waiting > 0:
            buffer += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

            # Process complete lines
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                line = line.strip()
                if line:
                    process_json_line(line, ser)

if __name__ == "__main__":
    main()
```

## Security Considerations

### Authentication
- Only READ_WRITE and ADMIN users can send `!app` commands
- User public key is provided in requests for application-level access control
- Applications can implement their own authorisation logic based on user identity

### Input Validation
- Application commands are user-controlled strings
- **Always validate and sanitise command inputs**
- Do not execute shell commands directly from user input
- Implement allowlists for valid commands

### Response Limits
- Keep responses concise (under 157 bytes recommended)
- Long responses may be truncated by packet size limits
- Consider pagination or summarisation for verbose output

### Error Handling
- Applications should respond within 10 seconds
- Invalid app_name requests are silently ignored (no route)
- Malformed appreply commands return errors to serial only

## Best Practices

1. **App Name Convention**: Use lowercase, descriptive names (e.g., "weather", "sensor", "control")
2. **Command Structure**: Keep commands simple and self-documenting
3. **Response Format**: Return human-readable text suitable for chat display
4. **Timeout Awareness**: Respond quickly or return partial results within 10 seconds
5. **Error Messages**: Return helpful error messages to users for invalid commands
6. **Logging**: Log all requests and responses for debugging and audit

## Limitations

- One pending request per user at a time
- No queuing of multiple requests from the same user
- No broadcasting (responses are unicast to requesting user only)
- No support for binary data (text only)
- Application must parse serial JSON in real-time (no REST API)

## JSON Output Format

For complete JSON schema documentation, see [json-output.md](json-output.md).
