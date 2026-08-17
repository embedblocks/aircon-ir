AC Control

ESP-IDF component for controlling air conditioners through infrared (IR).

The component separates AC operation, AC protocol encoding, and IR/RMT transmission so that additional AC brands/protocols can be added without changing the application-facing API or the IR hardware layer.
Architecture

Application    │    │ ac_command_t    ▼┌──────────────────────┐│   ac_operation.c     ││                      ││ Public AC API        │└──────────┬───────────┘           │           │ ac_command_t           ▼┌──────────────────────┐│   AC Protocol        ││                      ││  haier.c             ││  midea.c             ││  gree.c              ││  ...                 │└──────────┬───────────┘           │           │ ac_ir_frame_t           ▼┌──────────────────────┐│      ac_ir.c         ││                      ││ Generic IR TX        ││ ESP-IDF RMT          ││ 38 kHz carrier       │└──────────┬───────────┘           │           ▼        IR LED

The important boundary is:

     Protocol implementations generate a waveform. ac_ir.c transmits that waveform.
     Protocol implementations must not contain ESP-IDF RMT or GPIO transmission code.

Directory Structure
text
 
  
 
 
ac_control/
│
├── CMakeLists.txt
│
├── README.md
│
├── include/
│   └── ac_operation.h
│
└── src/
    │
    ├── ac_operation.c
    │
    ├── ac_protocol.h
    ├── ac_protocol_factory.c
    │
    ├── ac_ir.h
    ├── ac_ir.c
    ├── ac_ir_frame.h
    │
    └── protocols/
        ├── haier.h
        ├── haier.c
        │
        ├── midea.h
        ├── midea.c
        │
        └── ...
 
 
Public API

Only the files under include/ are public.

Application code should normally include:
c
 
  
 
 
#include "ac_operation.h"
 
 

The application should not need to know whether the AC uses Haier, Midea, Gree, Panasonic, etc.

Example:
c
 
  
 
 
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
 
 

Initialization:
c
 
  
 
 
ac_operation_init();
 
 

Basic power operation:
c
 
  
 
 
ac_switch_off();
 
 
ac_command_t

ac_command_t represents the desired AC operation, not an IR protocol.

Conceptually:
text
 
  
 
 
ac_command_t
    │
    ├── power
    ├── temperature
    ├── mode
    ├── fan
    ├── turbo
    ├── quiet
    ├── swing
    ├── sleep
    └── health
 
 

This structure should contain generic AC concepts.

Do not add fields such as:
c
 
  
 
 
midea_byte_3
haier_button
haier_checksum
 
 

Those belong inside the corresponding protocol implementation.
Operation Layer
ac_operation.c

This is the main control layer.

Its responsibility is:

     Initialize the selected protocol.
     Initialize the IR transmitter.
     Accept an ac_command_t.
     Ask the protocol implementation to encode it.
     Pass the resulting frame to ac_ir.c.

The flow is:
text
 
  
 
 
ac_set_operation(command)
        │
        ▼
protocol->encode(command, &frame)
        │
        ▼
ac_ir_send(&frame)
 
 

The operation layer should not know protocol-specific byte layouts or timings.
Protocol Layer
ac_protocol.h

The protocol interface is:
c
 
  
 
 
typedef struct {
    esp_err_t (*init)(void);

    esp_err_t (*encode)(
        const ac_command_t *command,
        ac_ir_frame_t *frame
    );
} ac_protocol_t;
 
 

Every AC protocol implementation must implement this interface.

For example:
c
 
  
 
 
const ac_protocol_t haier_protocol = {
    .init = haier_init,
    .encode = haier_encode,
};
 
 

or:
c
 
  
 
 
const ac_protocol_t midea_protocol = {
    .init = midea_init,
    .encode = midea_encode,
};
 
 
Adding a New AC Protocol

Suppose a new Gree protocol needs to be added.

Create:

     src/protocols/gree.h
     src/protocols/gree.c

gree.h
c
 
  
 
 
#pragma once

#include "ac_protocol.h"

extern const ac_protocol_t gree_protocol;
 
 
gree.c

The implementation should:
text
 
  
 
 
ac_command_t
      ↓
Gree protocol state
      ↓
Gree waveform
      ↓
ac_ir_frame_t
 
 

It should not call:

     rmt_new_tx_channel()
     rmt_transmit()
     gpio_set_level()

and should not contain carrier-generation code.

Its only job is to produce ac_ir_frame_t.
ac_ir_frame_t

Defined in src/ac_ir_frame.h:
c
 
  
 
 
typedef struct {
    uint16_t *durations;
    size_t count;
} ac_ir_frame_t;
 
 

The durations alternate:
text
 
  
 
 
durations[0] = MARK
durations[1] = SPACE
durations[2] = MARK
durations[3] = SPACE
...
 
 

     A mark means: IR carrier ON
     A space means: IR carrier OFF

The durations are expressed in microseconds.

Example:
text
 
  
 
 
560
560
560
1690
 
 

means:

     560 us MARK + 560 us SPACE → logical 0
     560 us MARK + 1690 us SPACE → logical 1

ac_ir_frame_t is intentionally protocol-independent.
IR Transmission Layer
ac_ir.c

ac_ir.c is responsible for the physical IR transmission.

It handles:

     ESP-IDF RMT TX channel
     RMT encoder
     Carrier generation
     38 kHz carrier
     Conversion of ac_ir_frame_t into RMT symbols
     Transmission completion

The protocol layer does not need to know anything about RMT.

The intended relationship is:
text
 
  
 
 
Protocol
    │
    │ durations in µs
    ▼
ac_ir_frame_t
    │
    ▼
ac_ir.c
    │
    │ RMT symbols
    ▼
ESP-IDF RMT
 
 

The RMT encoder may transmit a frame in multiple chunks when the hardware RMT memory cannot contain the entire waveform. This is completely internal to ac_ir.c.

Therefore the application still sees:
c
 
  
 
 
ac_set_operation(&command);
 
 

as one operation.
Carrier

The IR carrier is generated by the ESP-IDF RMT peripheral.

The current implementation uses:

     Carrier frequency: 38 kHz

Protocol implementations should specify mark/space durations, not individual 38 kHz pulses.

For example, a protocol should produce:
text
 
  
 
 
560   // mark
1690  // space
 
 

rather than trying to generate:
text
 
  
 
 
38 kHz
38 kHz
38 kHz
...
 
 

The RMT layer handles the carrier.
Protocol-Specific Example: Midea

The Midea protocol uses a 3-byte logical packet.

Each byte is transmitted followed by its bitwise complement:
text
 
  
 
 
Byte 0
~Byte 0

Byte 1
~Byte 1

Byte 2
~Byte 2
 
 

Therefore the transmitted payload is six bytes.

A known Midea format is:
[B2] [fan/state] [temperature/mode]

The first byte is: 0xB2

The protocol uses a variable-length pulse-distance encoding.

Conceptually:

     0: 1T MARK + 1T SPACE
     1: 1T MARK + 3T SPACE

The complete frame is transmitted twice for the normal command.

These details belong in src/protocols/midea.c. They should not appear in ac_ir.c.
Protocol-Specific Example: Haier

The Haier implementation similarly converts ac_command_t into its Haier-specific state bytes and then into ac_ir_frame_t.

The Haier protocol's byte layout, checksum, header and pulse timings remain entirely inside src/protocols/haier.c.

Adding another protocol must not require modifying ac_ir.c.
Protocol Selection
ac_protocol_factory.c

The factory determines which protocol implementation is currently used.

For example:
c
 
  
 
 
#include "ac_protocol.h"
#include "protocols/haier.h"

const ac_protocol_t *ac_protocol_get(void)
{
    return &haier_protocol;
}
 
 

To test Midea:
c
 
  
 
 
#include "ac_protocol.h"
#include "protocols/midea.h"

const ac_protocol_t *ac_protocol_get(void)
{
    return &midea_protocol;
}
 
 

Later this can be expanded if the product needs protocol selection based on configuration or hardware.
CMake

The component automatically includes all C sources under src/.

Example:
cmake
 
  
 
 
file(GLOB_RECURSE AC_CONTROL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)

idf_component_register(
    SRCS
        ${AC_CONTROL_SOURCES}

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "src"

    PRIV_REQUIRES
        driver
)
 
 

This means adding:

     src/protocols/midea.c
     src/protocols/gree.c
     src/protocols/panasonic.c

does not require manually editing CMakeLists.txt.
Rules for Adding a Protocol

When adding a new protocol, follow these rules.
1. Keep the application API generic

Do not expose protocol-specific fields through ac_command_t unless the feature genuinely exists across ACs.
2. Keep protocol logic in the protocol file

For example, midea.c should contain:

     Midea byte layout
     Midea checksum
     Midea temperature encoding
     Midea fan encoding
     Midea pulse timings

3. Never put RMT code in a protocol implementation

Do not use:

     rmt_new_tx_channel()
     rmt_transmit()
     rmt_apply_carrier()

inside:

     midea.c
     gree.c
     haier.c
     ...

4. Protocols return waveforms

The output of a protocol encoder is ac_ir_frame_t, not an RMT symbol array.
5. Keep the IR layer generic

ac_ir.c should work without knowing whether the frame came from:

     Haier
     Midea
     Gree
     Panasonic
     ...

6. Verify protocols against captures

For a new AC protocol, capture the original remote first.

Useful test data includes:

     Power
     Temperature +1
     Temperature -1
     Mode
     Fan
     Swing
     Turbo
     Sleep

Compare the captured waveform and decoded bytes against the generated frame.
Recommended Development Workflow

When adding a new protocol:
text
 
  
 
 
1. Capture original remote
        ↓
2. Identify protocol structure
        ↓
3. Determine byte/state fields
        ↓
4. Determine bit order
        ↓
5. Determine nominal timings
        ↓
6. Implement protocol encoder
        ↓
7. Produce ac_ir_frame_t
        ↓
8. Verify generated waveform
        ↓
9. Test against actual AC
 
 

Do not modify the RMT layer simply because a new protocol has different timings. The protocol encoder should express those timings in its generated frame.
Testing

A useful first test is to compare a known original remote transmission against the ESP32-generated transmission.

For example:
text
 
  
 
 
Original remote
       │
       ▼
IR receiver
       │
       ▼
raw durations / decoded bytes


ESP32 IR
       │
       ▼
IR receiver
       │
       ▼
raw durations / decoded bytes
 
 

The two should produce the same protocol-level message.

This separates:

     Protocol encoding problems
    from:
     RMT / carrier / hardware problems

Design Principle

The component intentionally follows this separation:
text
 
  
 
 
WHAT should the AC do?
        │
        ▼
   ac_command_t
        │
        ▼
HOW does this AC brand encode it?
        │
        ▼
   protocol implementation
        │
        ▼
WHAT waveform should be transmitted?
        │
        ▼
   ac_ir_frame_t
        │
        ▼
HOW is the waveform physically transmitted?
        │
        ▼
   ac_ir.c / ESP-IDF RMT
 
 

This allows new AC protocols to be added without changing application code or the physical IR transmission layer.

     

    Note: Keep this README at the component root as components/13_ac_control/README.md. It documents the architectural contract rather than tying the documentation to the current Haier/Midea implementations, which will make it useful when Gree/Panasonic/etc. are added.
    ```

    
     
