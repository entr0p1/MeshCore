# Overview

The Power Management module implements several protection mechanisms to preserve power in a low battery scenario and prevent flash corruption. 

Key features of this module are:
* **Power States**: Steady transition into varying levels of load shedding. States are transitioned based on battery voltage readings from the node.
* **Reset Reason Tracking**: Tracks the boot and shutdown reasons from `RESETREAS` and `GPREGRET`, respectively.
* **LPCOMP Wake**: If battery voltage is critically low and the node must shut down completely, a board reset occurs to automatically power the node back up when voltage returns to normal.
* **Boot Lockout**: If battery voltage is critically low when the board initially boots, it will configure LPCOMP to boot the boot again when voltage is normal and commence a shutdown. This feature is always bypassed if external power is present.

# Features

## States

Voltage measurements are taken on regular intervals during runtime and, if a threshold is crossed, the board will transition into gradually more conservative load shedding states.

### Normal

* Default state when on battery above thresholds, or whenever external power is detected.
* Full behaviour enabled (mesh processing, adverts, forwarding, sensors loop, etc.).
* Runtime voltage monitoring continues (every `PWRMGT_STATE_SCAN_INTVL` minutes).

### Conserve

Triggered when `VBAT < PWRMGT_VOLTAGE_CONSERVE` (but still above `PWRMGT_VOLTAGE_SLEEP`).

On transition to conserve:
* `PowerMgt::isInConserveMode()` is set to true
* Runtime load shedding is activated:
  * repeater: disables forwarding (`allowPacketForward()`)
  * Adverts suspended (`MyMesh::loop()`)
  * Guest logins silently rejected (to minimise unnecessary radio transmits)
  * Skip `sensors.loop()` in `main.cpp`
* Radio is still on and CPU is still running normally (no deep sleep / SYSTEMOFF)

Tuning:
* The state transition requires several consecutive scans (`PWRMGT_STATE_SCAN_DEBOUNCE`) at the scan cadence (`PWRMGT_STATE_SCAN_INTVL` minutes).

### Sleep

Triggered when `VBAT < PWRMGT_VOLTAGE_SLEEP` (but still above `PWRMGT_VOLTAGE_SHUTDOWN`).

On transition to sleep:
* Enter nRF52 backend `deepSleep()`
* Radio enters sleep state (`radio.powerOff()`) after waiting up to ~100 ms for any in-flight send to complete
* A wake cadence is scheduled using RTC2 compare for `PWRMGT_STATE_SCAN_INTVL` minutes
* The CPU blocks in `sd_app_evt_wait()` (if SoftDevice running) or WFE/SEV fallback otherwise
* `board.isInDeepSleep()` is called at the start of the main loop; if it returns true, the main loop skips mesh/sensors processing, so the device spends almost all its time halted

On wake interval:
* Syncs the firmware RTC time forward based on elapsed millis since last sleep
* Returns to sleep unless the state has been raised back to Normal/Conserve

On transition from sleep (to Conserve or Normal):
* Radio is reinitialised (`radio.begin()`)

### Shutdown

Triggered when `VBAT < PWRMGT_VOLTAGE_SHUTDOWN`.

On transition to shutdown:
* After debounce, `monitorVoltage()` immediately:
  1. Calls the board shutdown callback (`onShutdown`) so the variant can do hardware-specific prep (turn off peripherals, keep VBAT divider enabled, configure wake sources).
  2. Writes a shutdown reason into **GPREGRET** (`SHUTDOWN_REASON_LOW_VOLTAGE`).
  3. Enters **SYSTEMOFF** (`enterSystemOff()`).

In SYSTEMOFF:
* CPU stops and the device draws minimal current.
* It can only resume via a configured wake source, and resuming is a **reset**.

Note: "startup lockout" (boot voltage protection) is a separate early-boot path that also enters SYSTEMOFF, but with shutdown reason `SHUTDOWN_REASON_BOOT_PROTECT`.

---

## LPCOMP wake-from-SYSTEMOFF (battery recovery wake)

Allows the device to wake/reset automatically once battery voltage rises above a threshold (e.g., when external power is introduced to the board or the battery is charging).

LPCOMP wake is configured in (`configureLpcompWake()`) which:
* Uses **LPCOMP** on an analog input (AIN0–AIN7).
* Sets:
  * `PSEL` to the chosen AIN channel (VBAT-sense pin)
  * `REFSEL` to a fraction of VDD
  * `ANADETECT = Up` so it wakes on a rising crossing
  * `HYST = 50 mV` (at the comparator input) for noise immunity
  * Enables the `LPCOMP UP` interrupt

Important board requirements:
* The VBAT divider (if the board gates it) must remain **enabled during SYSTEMOFF** or LPCOMP will never see the battery rise. Some variants do this via `VBAT_ENABLE` which is held active in `prepareForBoardShutdown()`.

Threshold math (with a divider):
* LPCOMP compares the *ADC pin voltage* (VBAT after divider) to a reference derived from VDD
* If the battery sense is divided by `ADC_MULTIPLIER` (Typically ~3.0), then approximately:

`VBAT_wake ≈ VDD * (REF_FRACTION) * ADC_MULTIPLIER`

Essentially, you cannot set an exact millivolt wake point unless VDD is known and stable in SYSTEMOFF. Practically:

* Choose a fraction that makes `VBAT_wake`:
  * above `PWRMGT_VOLTAGE_SHUTDOWN` (to avoid chatter)
  * ideally at/above `PWRMGT_VOLTAGE_BOOTLOCK` (so you don’t wake and immediately lock out again)

Note: LPCOMP wake does **not** require SoftDevice; it’s hardware-driven as long as you actually enter SYSTEMOFF via a path that works without SoftDevice.

---

## External power behaviour

* `monitorVoltage()` checks `isExternalPowered()` and, when external power is detected, forces the state back to **Normal** and suppresses battery-driven state transitions while external power remains present.

---

## CLI

State and source:
* `get pwrmgt.state`: Returns the current power management state and timer of when transition to it occurred
* `get pwrmgt.laststate`: Returns the previous power management state and timer of when transition to it occurred
* `get pwrmgt.source`: Returns whether the board is currently powered by battery or external power
* `get pwrmgt.bootreason`: Returns the boot (reset) and shutdown reasons from the board, if available
* `get pwrmgt.avail`: Returns whether or not the board supports Power Management
* `get pwrmgt.bootmv`: Returns the voltage reading at boot in mV

Power Management Configuration:
* `get pwrmgt.enabled`: Get current state of Power Management state transition (disabled by default)
* `set pwrmgt.enabled on|off`: Enable or disable Power Management state transition (boot lockout still applies)

Manual forcing of states (for testing):
* `exec pwrmgt.conserve`
* `exec pwrmgt.sleep`
* `exec pwrmgt.shutdown` (WARNING: This **will** shut the board down, LPCOMP is enabled during this transition but avoid executing this on hard to reach nodes)


# Code Structure
* **States / Policy / Global API for all boards** (`PowerMgt`):
  `Normal -> Conserve -> Sleep -> Shutdown`
* **nRF52-specific backend implementation** (`Nrf52PowerMgt`):
  * runtime voltage monitoring + debounced state transitions
  * optional deep sleep cycle (RTC2-based wake cadence)
  * SYSTEMOFF entry + wake configuration
  * reset/shutdown reason helpers
* **Board wiring + behaviour** (variant + board class):
  battery ADC scaling, pins, wake source selection, “shutdown preparation”, etc.

### Hardware requirements (minimum)

To support *automatic* state changes and SYSTEMOFF recovery, the board needs:

1. **Battery voltage measurement** (an ADC pin tied to VBAT via divider or PMIC output)
2. **A way to wake from SYSTEMOFF**

   * Preferably **LPCOMP on the battery-sense pin** (VBAT recovery wake)
   * Optionally also **GPIO sense** (button wake)
3. Optional but recommended:

   * **External power detect** (VBUS detect, 5V rail sense, or PMIC status pin)
   * A way to **power down major peripherals** (GPS enable pin, sensor rail enable, radio power enable, etc.)

---

## Board Implementation

### 1) Add board-specific thresholds + battery measurement constants (`variants/<board>/variant.h`)

Add (or define) these macros in your board’s `variant.h`:

* Voltage thresholds (mV):

  * `PWRMGT_VOLTAGE_CONSERVE`
  * `PWRMGT_VOLTAGE_SLEEP`
  * `PWRMGT_VOLTAGE_SHUTDOWN`
  * `PWRMGT_VOLTAGE_BOOTLOCK` (0 to disable boot lockout)

* Battery ADC wiring:

  * `PIN_VBAT` or `PIN_VBAT_READ` (whatever your board uses)
  * `AREF_VOLTAGE` (if you use a fixed reference)
  * `ADC_MULTIPLIER` (VBAT scaling from ADC pin to VBAT in volts/mV)

* LPCOMP wake configuration:

  * `PWRMGT_LPCOMP_AIN` (AIN index 0–7)
  * `PWRMGT_LPCOMP_REF_*` (whatever your backend expects; currently your API takes an index that is written into `NRF_LPCOMP->REFSEL`)

**Rule of thumb:** Use a higher wake threshold than shutdown threshold and boot lockout threshold, so once the board wakes it is also allowed to boot.

### 2) Capture reset + shutdown reasons *before they get cleared* (`variants/<board>/early_boot.cpp` + `variant.cpp`)

The implementation uses two retained registers:

* `NRF_POWER->RESETREAS` (reset source)
* `NRF_POWER->GPREGRET` (the "shutdown reason" byte written before SYSTEMOFF)

For consistency across boards (use Xiao nRF52 as a reference):

* Add globals in `variant.cpp`:

  * `uint32_t g_reset_reason;`
  * `uint8_t g_shutdown_reason;`

* Add an `early_boot.cpp` with a high-priority constructor that reads:

  * `g_reset_reason = NRF_POWER->RESETREAS;`
  * `g_shutdown_reason = NRF_POWER->GPREGRET;`

* In `initVariant()` clear both registers for the *next* boot:

  * `NRF_POWER->RESETREAS = 0xFFFFFFFF;`
  * `NRF_POWER->GPREGRET = 0;`

Note: If you skip early capture, you will often misreport wake causes because these registers can be cleared/modified during init.

### 3) Implement board power management in the board class (`variants/<board>/<Board>.h/.cpp`)

Use `variants/xiao_nrf52/XiaoNrf52Board.*` as the reference shape.

Minimum additions:

**Includes**

* `#include <helpers/PowerMgt.h>`
* `#include <helpers/nrf52/Nrf52PowerMgt.h>`

**State**

* `PowerMgtState power_state;`
* `uint16_t boot_voltage_mv;`
* `uint32_t startup_reason;`
* `uint8_t shutdown_reason;`
* plus a `last_voltage_check_ms` for scan cadence.

**begin()**

* Call base init (`NRF52BoardDCDC::begin()` if applicable)
* Load reasons captured in early boot:
  * `startup_reason = g_reset_reason;`
  * `shutdown_reason = g_shutdown_reason;`
* Configure ADC for VBAT reading (resolution/reference consistent with your `getBattMilliVolts()`)
* Read `boot_voltage_mv = getBattMilliVolts()`
* **Boot lockout**:
  * If NOT externally powered and `boot_voltage_mv` is valid and `< PWRMGT_VOLTAGE_BOOTLOCK`:
    * call a board-specific `prepareForBoardShutdown()` that:
      * ensures the VBAT divider is enabled (if required)
      * configures LPCOMP wake (`configureLpcompWake(...)`)
      * powers down peripherals as needed
    * enter SYSTEMOFF with reason `SHUTDOWN_REASON_BOOT_PROTECT`
* Initialise `power_state` and timestamps
* `PowerMgt::setAvailable(true);`

**loop()**

* If PM is disabled/unavailable: keep state NORMAL and return
* Every `PWRMGT_STATE_SCAN_INTVL` minutes:
  * read current VBAT
  * call `Nrf52PowerMgt::monitorVoltage(&power_state, current_mv, onShutdownCallback)`
* Optional: state LED indicators (should be compiled out for production)

**isInDeepSleep()**

* return `Nrf52PowerMgt::deepSleep(&power_state, rtc_clock, radio_driver);`
* (only if you want sleep mode to actually shed load; otherwise omit/return false)

**setPwrMgtState() / powerOff()**

* `setPwrMgtState()` should call `transitionToState()` and if shutdown: run shutdown prep then SYSTEMOFF
* `powerOff()` should do the same, but as a user action (`SHUTDOWN_REASON_USER`) and optionally configure a GPIO button sense wake as well

### 4) External power detection

The nRF52 backend provides `Nrf52PowerMgt::isExternalPowered()` (VBUS detect via `USBREGSTATUS`).

For other boards:
* If the board has USB, `VBUSDETECT` may be sufficient.
* If the board is battery-only or has a PMIC with a “power good” pin, override `MainBoard::isExternalPowered()` and use the appropriate GPIO/status source.

This matters because `monitorVoltage()` currently suppresses “worse” state transitions while external power is present.

### 5) LPCOMP wake threshold (VBAT recovery wake)

### Constraint

LPCOMP wake is set as a **fraction of SoC VDD**, but the measured signal is **VBAT through a divider**. That means the effective VBAT wake point depends on:

* what VDD actually is in SYSTEMOFF on that board (regulated 3.0/3.3 vs other)
* the battery divider ratio (`ADC_MULTIPLIER`)

### Practical method

1. Determine the divider mapping:

* If `ADC_MULTIPLIER = 3.0`, then `Vpin ≈ VBAT / 3`.

2. Estimate VDD in SYSTEMOFF:

* Many boards keep VDD at the regulator output (commonly ~3.0–3.3V).

3. Choose a reference fraction that makes:

* `VBAT_wake ≈ VDD * fraction * ADC_MULTIPLIER`

4. Ensure:

* `VBAT_wake` is greater than `PWRMGT_VOLTAGE_BOOTLOCK` (or accept that it may wake then immediately lockout again)

For boards where VDD is uncertain, err slightly lower (more likely to wake), but keep it above the shutdown threshold to avoid chatter.

---

## Validation steps (per board)

1. Boot on battery above conserve threshold → state should be NORMAL.
2. Drop VBAT below conserve/sleep/shutdown thresholds (bench supply helps) -> confirm state transitions occur.
3. Confirm SYSTEMOFF entry (current draw should drop sharply).
4. Raise VBAT above wake threshold -> confirm wake/reset occurs.
5. Confirm `RESETREAS` and `GPREGRET` reporting matches expectations.

---