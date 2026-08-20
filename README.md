# aircon-ir

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

ESP-IDF component for controlling air conditioners over **infrared (IR)** using the ESP32 **RMT** peripheral.

Separates AC operation, AC protocol encoding, and IR/RMT transmission into independent layers, so additional AC brands/protocols can be added without changing the application-facing API or the IR hardware layer.

---

## Features

* Common `ac_command_t` application API — independent of AC brand
* Protocol implementations for **Gree**, **Haier**, and **Midea**
* Generic IR transmission layer built on ESP-IDF's RMT simple encoder
* Correctly handles waveforms larger than the RMT hardware memory (multi-chunk transmission)
* Correctly handles individual mark/space durations longer than the RMT hardware's 15-bit duration field (automatic splitting)
* Hardware-generated 38 kHz IR carrier
* Includes a working example and an interactive Unity-based test app that turns a serial terminal into an AC remote control

---

## Chip Support

| Chip     | Status |
|----------|--------|
| ESP32-C3 | ✅ Tested |
| ESP32    | ⚠️ Expected to work |
| ESP32-S2 | ⚠️ Expected to work |
| ESP32-S3 | ⚠️ Expected to work |

---

## Installation

### Using ESP-IDF Component Manager (Recommended)

```bash
idf.py add-dependency "emmbedblocks/aircon-ir^0.1.1"
```

Or in your project's `idf_component.yml`:

```yaml
dependencies:
  emmbedblocks/aircon-ir: "^0.1.1"
```

---

## Usage

Select the target AC protocol and IR transmitter GPIO before building (protocol defaults to Midea, GPIO defaults to 18, if skipped):

```
idf.py menuconfig
Component config → AC Control → AC IR Protocol
Component config → AC Control → IR Transmitter GPIO
```

> **Wiring note:** the configured GPIO only *switches* the IR LED (through a transistor/MOSFET driver stage, as is standard for IR blasters) — it does not supply its power directly. Do not wire the IR LED to be powered straight from the GPIO pin; the pin doesn't source enough current to drive an IR LED at useful range, and doing so risks damaging the pin.

```c
#include "ac_operation.h"

ac_operation_init();

ac_command_t command = {
    .power = true,
    .temperature = 24,
    .mode = AC_MODE_COOL,
    .fan = AC_FAN_HIGH,

    .turbo = false,
    .quiet = false,

    .swing_v = AC_SWING_V_OFF,
    .swing_h = AC_SWING_H_MIDDLE,

    .sleep = false,
    .health = false,
};

esp_err_t ret = ac_set_operation(&command);
```

Basic power operation:

```c
ac_switch_off();
```

Application code only ever includes `ac_operation.h` and never needs to know whether the target AC is Haier, Midea, Gree, etc.

> **Swing is unverified.** `ac_command_t` exposes `swing_v`/`swing_h`, but swing was not part of the original capture set (see [Data Provenance](#data-provenance)). Don't rely on it in a real deployment without capturing and checking it yourself first.

---

## Architecture

```text
Application
    │
    │ ac_command_t
    ▼
ac_operation.c           Public AC API
    │
    │ ac_command_t
    ▼
Protocol layer            gree.c / haier.c / midea.c / ...
    │
    │ ac_ir_frame_t
    ▼
ac_ir.c                   Generic IR TX (ESP-IDF RMT, 38 kHz carrier)
    │
    ▼
IR LED
```

The important boundary:

* Protocol implementations generate a **waveform** (`ac_ir_frame_t`).
* `ac_ir.c` **transmits** that waveform.
* Protocol implementations must never contain ESP-IDF RMT or GPIO transmission code.

### Design Principle

```text
WHAT should the AC do?                        ac_command_t
        ↓
HOW does this AC brand encode it?             protocol implementation
        ↓
WHAT waveform should be transmitted?          ac_ir_frame_t
        ↓
HOW is the waveform physically transmitted?   ac_ir.c / ESP-IDF RMT
```

This separation allows new AC protocols to be added without changing application code or the physical IR transmission layer, and allows the IR transmission layer to be hardened (e.g. RMT memory handling, long-duration splitting) without touching any protocol implementation.

### Directory Structure

```text
aircon-ir/
│
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── README.md
│
├── include/
│   └── ac_operation.h        Public, application-facing API
│
├── src/
│   ├── ac_operation.c        Operation layer (init, encode, transmit)
│   │
│   ├── ac_protocol.h         Protocol interface (init/encode)
│   ├── ac_protocol_factory.c Selects the active protocol via Kconfig
│   │
│   ├── ac_ir.h
│   ├── ac_ir.c                Generic IR TX — ESP-IDF RMT, 38 kHz carrier
│   ├── ac_ir_frame.h          Protocol-independent waveform type
│   │
│   └── protocols/
│       ├── gree.h / gree.c
│       ├── haier.h / haier.c
│       └── midea.h / midea.c
│
├── example/
│   └── simple/                Standalone example project
│
└── test_apps/
    └── remote_control/        Interactive Unity test menu (serial-terminal remote control)
```

Only `include/` is public. Everything under `src/` — including the protocol implementations — is private to the component.

---

## Public API

Only `include/ac_operation.h` is public application-facing API:

```c
#include "ac_operation.h"
```

`ac_command_t` represents the desired AC *operation*, not an IR protocol — it should only ever contain generic AC concepts (power, temperature, mode, fan, turbo, quiet, swing, sleep, health), never protocol-specific fields such as a Midea byte index or a Haier checksum. Those belong inside the corresponding protocol implementation. For example, fields such as `midea_byte_3` or `haier_button` must never be added to `ac_command_t` — if a concept genuinely doesn't exist across AC brands, it stays inside that brand's protocol file instead of leaking into the generic API.

---

## Protocol Layer

Every AC protocol implementation conforms to:

```c
typedef struct {
    esp_err_t (*init)(void);

    esp_err_t (*encode)(
        const ac_command_t *command,
        ac_ir_frame_t *frame
    );
} ac_protocol_t;
```

For example:

```c
const ac_protocol_t haier_protocol = {
    .init = haier_init,
    .encode = haier_encode,
};
```

A protocol implementation's only job is `ac_command_t` → `ac_ir_frame_t`. It must never call `rmt_new_tx_channel()`, `rmt_transmit()`, `gpio_set_level()`, or generate carrier pulses directly — the RMT layer handles the carrier, protocols only specify mark/space durations in microseconds.

### `ac_ir_frame_t`

```c
typedef struct {
    uint32_t *durations;
    size_t count;
} ac_ir_frame_t;
```

Durations alternate mark/space, expressed in microseconds:

```text
durations[0] = MARK
durations[1] = SPACE
durations[2] = MARK
durations[3] = SPACE
...
```

For example, `560, 560, 560, 1690` encodes two bits: a `560` mark + `560` space is a logical `0`, and a `560` mark + `1690` space is a logical `1`. `ac_ir_frame_t` is intentionally protocol-independent, and its durations are not constrained by any RMT hardware limit — `ac_ir.c` is responsible for adapting arbitrary logical durations to the hardware representation.

### Data Provenance

Each protocol was reverse-engineered from real remote captures, but not every `ac_command_t` field was part of that capture set. This matters because a field that "compiles and sends a frame" is not the same as a field that has been checked against a real remote.

**Captured and verified against real remote transmissions**, for all three protocols:

* Power (on/off)
* Mode (cool, heat, dry, auto)
* Temperature
* Fan speed (low, medium, high, auto)

**Not captured — swing (vertical and horizontal) was intentionally left out of the capture set** as it was judged non-essential at the time. As a result:

* **Midea** — `midea.c` does not implement swing at all. `command->swing_v` and `command->swing_h` are silently ignored; nothing is encoded into the transmitted frame for them.
* **Haier / Gree** — swing *is* encoded into the frame, but the bit values (`HAIER_SWING_V_*`, `HAIER_SWING_H_*`, `GREE_SWING_V_*`, `GREE_SWING_H_*`) are best-effort guesses based on typical layouts for that vendor, not values confirmed from a capture. They may be wrong, partially wrong, or map to the wrong physical position.

If your application needs reliable swing control, capture it yourself (see [Running Tests](#running-tests)) and correct the relevant `protocols/<name>.c` before relying on it.

### Protocol-Specific Notes

These are examples of how much protocol detail is expected to live *inside* a protocol file, and never leak into `ac_operation.c` or `ac_ir.c`:

**Midea** — a 3-byte logical packet (`[0xB2] [fan/state] [temperature/mode]`), where each byte is transmitted followed by its bitwise complement (so the transmitted payload is six bytes: `Byte0 ~Byte0 Byte1 ~Byte1 Byte2 ~Byte2`), using pulse-distance encoding (`0` = 1T mark + 1T space, `1` = 1T mark + 3T space), with the complete frame transmitted twice per command.

**Haier** — a 14-byte state block with its own header timing, per-bit pulse-distance encoding, a trailing mark, and a modulo-256 checksum over bytes 0–12. The byte layout, checksum, header, and pulse timings are entirely internal to `haier.c`.

### Protocol Selection

The active protocol is selected via Kconfig:

```
idf.py menuconfig
Component config → AC Control → AC IR Protocol
    ( ) Midea
    ( ) Gree
    ( ) Haier
```

Internally, `ac_protocol_factory.c` returns the protocol descriptor selected by Kconfig:

```c
#include "ac_protocol.h"
#include "protocols/midea.h"
#include "protocols/gree.h"
#include "protocols/haier.h"

const ac_protocol_t *ac_protocol_get(void)
{
#if CONFIG_AC_PROTOCOL_MIDEA
    return &midea_protocol;
#elif CONFIG_AC_PROTOCOL_GREE
    return &gree_protocol;
#elif CONFIG_AC_PROTOCOL_HAIER
    return &haier_protocol;
#else
#error "No AC protocol selected"
#endif
}
```

All protocol sources are compiled in regardless of selection; the linker's `--gc-sections` behavior (ESP-IDF's default) discards the unreferenced protocol descriptors from the final binary, so switching protocols is a `menuconfig` + rebuild, not a source edit.

Adding a new protocol still means adding a `choice` entry to the component's `Kconfig` and a case to this `#if`/`#elif` chain — a self-registration mechanism that would remove even that step is on the roadmap (see Known Limitations).

### Adding a New Protocol

```text
1. Capture original remote
        ↓
2. Identify protocol structure
        ↓
3. Determine byte/state fields, bit order, nominal timings
        ↓
4. Implement protocol encoder → ac_ir_frame_t
        ↓
5. Verify generated waveform against the capture
        ↓
6. Test against the actual AC
```

Create `src/protocols/<name>.h` and `src/protocols/<name>.c`. The component's `CMakeLists.txt` globs all sources under `src/`, so a new protocol file is picked up automatically without editing `CMakeLists.txt`.

Rules to follow:

1. **Keep the application API generic.** Do not expose protocol-specific fields through `ac_command_t` unless the feature genuinely exists across ACs.
2. **Keep protocol logic in the protocol file** — byte layout, checksum, temperature/fan/mode encoding, and pulse timings all belong in e.g. `midea.c`, not in `ac_operation.c`.
3. **Never put RMT code in a protocol implementation.** No `rmt_new_tx_channel()`, `rmt_transmit()`, or `rmt_apply_carrier()` inside any file under `protocols/`.
4. **Protocols return waveforms.** The output of a protocol encoder is `ac_ir_frame_t`, not an RMT symbol array.
5. **Keep the IR layer generic.** `ac_ir.c` must work without knowing whether the frame came from Haier, Midea, Gree, Panasonic, or anything else.
6. **Verify against captures.** Capture the original remote first and compare it against the generated frame (see Testing below).

Adding a new protocol must never require modifying `ac_ir.c`.

---

## IR Transmission Layer (`ac_ir.c`)

Handles everything protocol implementations must not:

* ESP-IDF RMT TX channel and encoder setup
* 38 kHz carrier generation
* Conversion of `ac_ir_frame_t` into RMT symbols, including:
  * Splitting individual logical durations longer than the RMT hardware's 15-bit duration field into multiple same-level RMT symbols
  * Producing waveforms larger than the RMT hardware memory across multiple driver refill callbacks
* Transmission completion (`ac_set_operation()` is synchronous from the application's point of view, regardless of how many chunks the transmission required internally)

---

## Examples

### `example/simple`

Walks through the full application-facing API against a single project: init, a full power-on command, cycling through modes (cool/heat/dry/auto), adjusting temperature and fan speed, toggling turbo/quiet/sleep/health, and powering off with `ac_switch_off()`. It also demonstrates `swing_v`/`swing_h` in one step, clearly marked as unverified (see [Data Provenance](#data-provenance)) rather than as a confirmed feature.

---

## Running Tests

```bash
cd test_apps/remote_control
idf.py build flash monitor
```

This flashes an interactive Unity test menu that turns the device's serial terminal into an AC remote control — each test case sends one real IR command, so you can drive the AC directly from the monitor instead of writing application code. Current menu entries:

| Menu entry | Effect |
|---|---|
| Power ON | Powers the AC on (cool, 26 °C, auto fan) |
| Power OFF | Powers the AC off |
| Set Mode: COOL | Switches to cool mode |
| Set Mode: HEAT | Switches to heat mode |
| Increase Fan Speed | Cycles fan speed up (auto → low → medium → high → auto) |
| Decrease Fan Speed | Cycles fan speed down (auto → high → medium → low → auto) |
| Increase Temperature | +1 °C, capped at 30 °C |
| Decrease Temperature | −1 °C, capped at 17 °C |

For protocol-level verification (step 6 in "Adding a New Protocol"), compare a capture of the original remote against a capture of the ESP32's transmission, using an IR receiver on both:

```text
Original remote → IR receiver → raw durations / decoded bytes
ESP32 IR        → IR receiver → raw durations / decoded bytes
```

The two should decode to the same protocol-level message. This keeps protocol-encoding bugs separate from RMT/carrier/hardware bugs.

---

## Known Limitations

* **Swing (`swing_v`/`swing_h`) is unverified** — see [Data Provenance](#data-provenance). Not implemented at all for Midea; best-effort/unconfirmed for Haier and Gree.
* Adding a new protocol currently requires a `Kconfig` `choice` entry plus a matching `#elif` branch in `ac_protocol_factory.c`; a self-registering protocol registry (so the factory needs no changes at all) is on the roadmap
* Tested primarily on ESP32-C3; other targets are expected to work but not yet verified

---

## License

MIT License — see LICENSE file.
