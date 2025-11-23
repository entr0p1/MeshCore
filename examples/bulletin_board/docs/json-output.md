# JSON Serial Output Format

The bulletin board server outputs structured JSON to the serial console for all significant events. This allows external applications to monitor activity and integrate with the mesh network.

## JSON Structure

All JSON output follows a consistent schema:

```json
{
  "component": "<component_name>",
  "action": "<action_name>",
  "data": {
    "<data_fields>"
  },
  "meta": {
    "<metadata_fields>"
  }
}
```

### Top-Level Fields

- **component** (string): Source component or subsystem
- **action** (string): Action or event type
- **data** (object): Component-specific data payload
- **meta** (object): Common metadata fields

### Data Fields

Varies by component/action. Common data fields:
- **type** (string): Event sub-type or data classification
- **severity** (string): Severity level for bulletins (info/warning/critical)
- **text** (string): Message or response text
- **mode** (string): Configuration mode
- **app_name** (string): External application identifier
- **command** (string): Command string

### Meta Fields

Common metadata included in most events:
- **user_pubkey** (string): 64-char hex (32 bytes) user public key (if applicable)
- **user_role** (string): User permission level (guest/read_write/admin)
- **source** (string): Event origin (mesh/console/system)
- **timestamp** (integer): Unix timestamp

## Component Types

### post

Message and bulletin creation events.

#### create (action)

**Data fields:**
- `type`: "message" (user post) or "bulletin" (server post)
- `severity`: "info", "warning", or "critical" (bulletin only)
- `text`: Post content

**Meta fields:**
- `user_pubkey`: Author's public key (message only, NULL for bulletin)
- `source`: "mesh" (user post) or "console" (server bulletin)
- `timestamp`: Post creation timestamp

**Examples:**

User message:
```json
{
  "component": "post",
  "action": "create",
  "data": {
    "type": "message",
    "text": "Hello everyone!"
  },
  "meta": {
    "user_pubkey": "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6A7B8C9D0E1F2G3H4I5J6K7L8M9N0O5P6",
    "source": "mesh",
    "timestamp": 1735689600
  }
}
```

Server bulletin:
```json
{
  "component": "post",
  "action": "create",
  "data": {
    "type": "bulletin",
    "severity": "critical",
    "text": "EVACUATION REQUIRED"
  },
  "meta": {
    "user_pubkey": null,
    "source": "console",
    "timestamp": 1735689600
  }
}
```

### app

External application request/response events.

#### request (action)

**Data fields:**
- `app_name`: Target application identifier
- `command`: Command to execute

**Meta fields:**
- `user_pubkey`: Requesting user's public key
- `source`: Always "mesh"
- `timestamp`: Request timestamp

**Example:**
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

#### response (action)

**Data fields:**
- `type`: Always "data"
- `app_name`: Application identifier
- `text`: Response text

**Meta fields:**
- `user_pubkey`: Target user's public key
- `source`: Always "console"
- `timestamp`: Response timestamp

**Example:**
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

### system

System-level events and notifications.

#### notification (action)

**Data fields:**
- `type`: Always "message"
- `severity`: NULL (not applicable)
- `text`: System notification text

**Meta fields:**
- `user_pubkey`: NULL (system-generated)
- `user_role`: Always "system"
- `source`: Always "system"
- `timestamp`: Event timestamp

**Example:**
```json
{
  "component": "system",
  "action": "notification",
  "data": {
    "type": "message",
    "severity": null,
    "text": "Clock synced by admin [A1B2C3D4]. Server now in read-write mode."
  },
  "meta": {
    "user_pubkey": null,
    "user_role": "system",
    "source": "system",
    "timestamp": 1735689600
  }
}
```

### login

Client authentication events.

#### success (action)

**Data fields:**
- `type`: Always "auth"
- `severity`: NULL
- `text`: Login success message with permission level

**Meta fields:**
- `user_pubkey`: Client's public key
- `user_role`: Granted permission level (guest/read_write/admin)
- `source`: Always "mesh"
- `timestamp`: Login timestamp

**Example:**
```json
{
  "component": "login",
  "action": "success",
  "data": {
    "type": "auth",
    "severity": null,
    "text": "Login OK"
  },
  "meta": {
    "user_pubkey": "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6A7B8C9D0E1F2G3H4I5J6K7L8M9N0O5P6",
    "user_role": "admin",
    "source": "mesh",
    "timestamp": 1735689600
  }
}
```

### config

Configuration change events.

#### update (action)

**Data fields:**
- `type`: Configuration type (e.g., "channel")
- `mode`: Configuration mode (e.g., "private", "public")
- Additional fields specific to config type

**Example (channel config):**
```json
{
  "component": "config",
  "action": "update",
  "data": {
    "type": "channel",
    "mode": "private",
    "key": "A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6"
  }
}
```

Note: Config events may not include meta fields.

## Source Types

- **mesh**: Event originated from mesh network (user action)
- **console**: Event originated from serial console or external app
- **system**: Event generated by server itself (e.g., boot, clock sync)

## User Roles

- **guest**: Guest access (read-only, passwordless)
- **read_write**: Standard user (can post)
- **admin**: Administrator (full privileges)
- **system**: System-generated event (not a user)

## Severity Levels

Used for bulletin posts only:
- **info**: Informational bulletin (local only)
- **warning**: Warning bulletin (local + broadcast)
- **critical**: Critical bulletin (local + broadcast)

## Parsing Recommendations

1. **Line-based parsing**: Each JSON object is output on a single line
2. **Robust error handling**: Non-JSON lines may appear (debug output, CLI prompts)
3. **Buffer management**: Use line buffering to handle partial reads
4. **Field validation**: Check for presence of expected fields before accessing
5. **Timestamp handling**: Unix timestamps are 32-bit integers (valid until 2106)

## Example Parser (Python)

```python
import json

def parse_json_line(line):
    """Parse a single JSON line from serial output"""
    try:
        data = json.loads(line)
        component = data.get("component")
        action = data.get("action")

        # Route based on component/action
        if component == "post" and action == "create":
            handle_post_create(data)
        elif component == "app" and action == "request":
            handle_app_request(data)
        elif component == "system" and action == "notification":
            handle_system_notification(data)
        # ... etc

    except json.JSONDecodeError:
        # Not JSON, ignore
        pass
    except KeyError as e:
        # Missing expected field
        print(f"Malformed JSON: missing {e}")

def handle_post_create(data):
    """Handle post creation event"""
    post_type = data["data"]["type"]
    text = data["data"]["text"]
    timestamp = data["meta"]["timestamp"]

    if post_type == "bulletin":
        severity = data["data"]["severity"]
        print(f"[{timestamp}] BULLETIN ({severity}): {text}")
    else:
        user = data["meta"]["user_pubkey"][:8]  # First 4 bytes
        print(f"[{timestamp}] {user}: {text}")
```

## Completeness

This document covers the primary JSON output events. Additional events may be added in future versions. Always validate field existence before accessing to maintain forward compatibility.
