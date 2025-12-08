#pragma once

#include <stdint.h>

// Lightweight, platform-neutral power management facade.
// Backends (e.g. nRF52) drive availability and state; callers query policy.
namespace PowerMgt {

// Generic power states
enum PowerState : uint8_t {
  STATE_NORMAL = 0,
  STATE_CONSERVE = 1,
  STATE_SLEEP = 2,
  STATE_SHUTDOWN = 3
};

// Backend signals that power management is available on this board/firmware
void setAvailable(bool available);
bool isAvailable();

// Firmware preference to enable/disable runtime power management (startup lockout exempt)
void setRuntimeEnabled(bool enabled);
bool isRuntimeEnabled();

// Backend updates current state; callers can query policy helpers
void setState(uint8_t state);
uint8_t getState();

// Human-readable state name
const char* getStateString(uint8_t state);

// Returns true if power management is active and restricting operations
inline bool isInConserveMode() {
  return isAvailable() && isRuntimeEnabled() && (getState() >= STATE_CONSERVE);
}

} // namespace PowerMgt

