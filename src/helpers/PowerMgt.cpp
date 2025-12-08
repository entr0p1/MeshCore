#include "PowerMgt.h"

namespace PowerMgt {

// Internal state
static bool g_available = false;
static bool g_runtime_enabled = false;
static uint8_t g_state = STATE_NORMAL;

void setAvailable(bool available) {
  g_available = available;
}

bool isAvailable() {
  return g_available;
}

void setRuntimeEnabled(bool enabled) {
  g_runtime_enabled = enabled;
  if (!enabled) {
    // When disabled, always present NORMAL to callers
    g_state = STATE_NORMAL;
  }
}

bool isRuntimeEnabled() {
  return g_runtime_enabled;
}

void setState(uint8_t state) {
  // Only accept backend updates when available and runtime enabled
  if (!g_available || !g_runtime_enabled) {
    g_state = STATE_NORMAL;
    return;
  }
  g_state = state;
}

uint8_t getState() {
  if (!g_available || !g_runtime_enabled) {
    return STATE_NORMAL;
  }
  return g_state;
}

const char* getStateString(uint8_t state) {
  switch(state) {
    case STATE_NORMAL:    return "Normal";
    case STATE_CONSERVE:  return "Conserve";
    case STATE_SLEEP:     return "Sleep";
    case STATE_SHUTDOWN:  return "Shutdown";
    default:              return "Unknown";
  }
}

} // namespace PowerMgt
