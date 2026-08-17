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
* Includes a working example and a Pytest-based hardware transport test app

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
idf.py add-dependency "emmbedblocks/aircon-ir^0.1.0"
```

Or in your project's `idf_component.yml`:

```yaml
dependencies:
  emmbedblocks/aircon-ir: "^0.1.0"
```

---

## Usage

Select the target AC protocol before building (defaults to Midea if skipped):

```
idf.py menuconfig
Component config → AC Control → AC IR Protocol
```

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

---

## Public API

Only `include/ac_operation.h` is public application-facing API:

```c
#include "ac_operation.h"
```

`ac_command_t` represents the desired AC *operation*, not an IR protocol — it should only ever contain generic AC concepts (power, temperature, mode, fan, turbo, quiet, swing, sleep, health), never protocol-specific fields such as a Midea byte index or a Haier checksum. Those belong inside the corresponding protocol implementation.

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

`ac_ir_frame_t` is intentionally protocol-independent, and its durations are not constrained by any RMT hardware limit — `ac_ir.c` is responsible for adapting arbitrary logical durations to the hardware representation.

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

Minimal example showing initialization and sending a basic command.

---

## Running Tests

```bash
cd test_apps/transport
idf.py build flash monitor
```

The transport test app includes Pytest-based waveform verification against expected mark/space timings.

---

## Design Principle

```text
WHAT should the AC do?              ac_command_t
        ↓
HOW does this AC brand encode it?   protocol implementation
        ↓
WHAT waveform should be transmitted?  ac_ir_frame_t
        ↓
HOW is the waveform physically transmitted?  ac_ir.c / ESP-IDF RMT
```

This separation allows new AC protocols to be added without changing application code or the physical IR transmission layer, and allows the IR transmission layer to be hardened (e.g. RMT memory handling, long-duration splitting) without touching any protocol implementation.

---

## Known Limitations

* Adding a new protocol currently requires a `Kconfig` `choice` entry plus a matching `#elif` branch in `ac_protocol_factory.c`; a self-registering protocol registry (so the factory needs no changes at all) is on the roadmap
* Tested primarily on ESP32-C3; other targets are expected to work but not yet verified

---

## License

MIT License — see LICENSE file.