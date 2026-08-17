# AC IR RMT Transport Layer — Hardware Verification Test Requirements

## 1. Purpose

Create an automated hardware verification test for the ESP-IDF `ac_ir` RMT transmission layer.

The purpose of this test is **not** to verify the Haier AC protocol, Haier frame encoding, checksum, or whether an actual AC accepts the transmitted signal.

The purpose is strictly to verify that:

> Given an `ac_ir_frame_t` containing a known sequence of durations, the `ac_ir` RMT transport layer physically transmits that same duration sequence, in the same order, without losing, duplicating, reordering, or otherwise corrupting durations across multiple RMT encoder invocations.

The `ac_ir` transport layer must be treated as **protocol-independent**.

---

# 2. System Under Test

The supplied:

* `ac_ir.c`
* `ac_ir.h`

are the system under test.

The test must exercise the real production implementation, including:

* `ac_ir_init()`
* `ac_ir_send()`
* the custom RMT encoder
* the ESP-IDF RMT TX driver
* the configured RMT carrier
* the actual RMT TX GPIO

Do not replace the production RMT implementation with a mock for the primary hardware test.

The target MCU for the current test is:

> **ESP32-C3**

The test generator must inspect the supplied `ac_ir.c` and `ac_ir.h` and use the actual definitions and APIs found there.

Do not invent an alternative `ac_ir_frame_t` definition if `ac_ir.h` is supplied.

---

# 3. Scope

The test covers the complete physical transport path:

```text
Known test duration array
        ↓
ac_ir_frame_t
        ↓
ac_ir_send()
        ↓
custom RMT encoder
        ↓
ESP-IDF RMT driver
        ↓
RMT TX GPIO
        ↓
logic analyzer
        ↓
Python waveform extraction
        ↓
expected vs actual comparison
```

The test specifically targets the custom encoder's **multi-invocation behavior**.

The test frame must contain more than `AC_IR_ENCODER_SYMBOLS` symbols so that the custom encoder has to continue the transmission across multiple encoder invocations.

---

# 4. Existing Implementation Characteristics

The supplied `ac_ir.c` currently defines:

```c
#define AC_IR_RESOLUTION_HZ    1000000
#define AC_IR_CARRIER_FREQ_HZ  38000
#define AC_IR_CARRIER_DUTY     0.33f
#define AC_IR_ENCODER_SYMBOLS  32
```

The RMT TX configuration currently contains:

```c
.mem_block_symbols = 48
```

The TX GPIO is currently:

```c
#define AC_IR_TX_GPIO 10
```

The custom encoder converts two duration entries into one RMT symbol:

```text
duration[0] + duration[1] → symbol 0
duration[2] + duration[3] → symbol 1
duration[4] + duration[5] → symbol 2
...
```

The encoder generates at most 32 symbols per invocation because:

```c
#define AC_IR_ENCODER_SYMBOLS 32
```

This is the relevant multi-invocation boundary for this test.

**Do not describe this as an RMT-memory limitation.**

The RMT memory configuration is 48 symbols, while the custom encoder intentionally limits each batch to 32 symbols.

The test is therefore intended to verify:

> **behavior when the waveform contains more than 32 symbols and the custom encoder must continue across multiple invocations.**

The exact number of encoder invocations is not itself an acceptance criterion. The acceptance criterion is the correctness of the complete physical waveform.

---

# 5. Primary Test Frame

The primary test must contain exactly:

```text
112 durations
```

The encoder interprets these as:

```text
112 durations
      ↓
56 RMT symbols
```

Because 56 > 32, the test necessarily exercises the encoder's multi-invocation path.

The intended logical split is:

```text
symbols 0–31
symbols 32–55
```

However, the test must **not** require the ESP-IDF RMT driver to make exactly two calls to the encoder.

The driver may invoke the encoder according to its own refill behavior.

What matters is that the complete output contains all 56 symbols / 112 durations correctly.

---

# 6. Test Data

The test data must be deterministic and deliberately recognizable.

Do not use random values.

Use a monotonically increasing sequence where each duration can be identified by its position.

For example:

```text
1000, 1010,
1020, 1030,
1040, 1050,
...
```

Continue the sequence until there are exactly 112 duration values.

The exact starting value and increment may be adjusted if necessary, but the generated values must satisfy these requirements:

1. Exactly 112 values.
2. Preferably every value is unique.
3. Values must be comfortably measurable by the logic analyzer.
4. Values must be valid for the actual duration element type defined by `ac_ir.h`.
5. Values must be valid for the RMT duration field.
6. Values must not depend on Haier protocol semantics.
7. The sequence must be generated deterministically.
8. The same expected sequence must be available to pytest.

The suggested 1000–2110 µs range is preferred because it is comfortably below the 15-bit RMT duration limit and well within the existing `rmt_tx_wait_all_done()` timeout.

---

# 7. Waveform Semantics

The supplied duration array represents alternating mark and space durations:

```text
duration[0] = mark
duration[1] = space
duration[2] = mark
duration[3] = space
...
duration[110] = mark
duration[111] = space
```

The required physical envelope is therefore:

```text
D[0]
D[1]
D[2]
...
D[111]
```

in exactly that order.

The semantic meaning of the durations is irrelevant.

They are simply test markers used to verify transport integrity.

---

# 8. Required ESP-IDF Test Firmware

Create a minimal ESP-IDF test application/firmware that:

1. Targets ESP32-C3.
2. Uses the supplied production `ac_ir.c` / `ac_ir.h`.
3. Calls the real `ac_ir_init()`.
4. Creates the deterministic 112-duration test array.
5. Creates an `ac_ir_frame_t` using that array.
6. Calls the real `ac_ir_send()`.
7. Waits for the real transmission to finish through the existing synchronous API.
8. Provides clear serial markers indicating when the test transmission begins and ends.
9. Does not use `haier.c`.
10. Does not generate a Haier protocol frame.
11. Does not alter or encode the test durations before passing them to `ac_ir_send()`.

The serial output should provide enough information to identify a capture, for example:

```text
AC_IR_TRANSPORT_TEST_START
DURATION_COUNT=112
EXPECTED_SYMBOL_COUNT=56
AC_IR_TRANSPORT_TEST_TX_DONE
AC_IR_TRANSPORT_TEST_END
```

The exact format may be adapted to the existing project.

The serial output is **not** the waveform verification mechanism.

---

# 9. Physical Measurement

Connect a logic analyzer to the actual RMT TX GPIO used by the production source.

For the currently supplied source this is:

```text
GPIO10
```

Capture the complete physical transmission.

The test must verify the actual GPIO output rather than relying on:

* ESP-IDF log messages
* internal encoder variables
* debug counters
* manually inspected logs

The logic analyzer capture is the authoritative measurement of the transmitted waveform.

> **Note (deviation from this original section):** this implementation replaces the external logic analyzer with an on-chip loopback: the TX GPIO is jumpered to a second GPIO configured as a plain interrupt-capable input (not RMT), and every edge is timestamped in firmware with `esp_timer_get_time()`. This still satisfies the intent of this section — the measurement is of the actual physical GPIO output, independent of `ac_ir.c`'s internal encoder state — while removing the dependency on an external tool and capture-file format. See the top-level README for the rationale.

---

# 10. Carrier

The production implementation applies:

```text
38 kHz carrier
33% duty cycle
```

using the ESP-IDF RMT carrier configuration.

The custom encoder produces:

```text
level0 = 1
level1 = 0
```

for each symbol.

Therefore the physical GPIO is expected to contain approximately 38-kHz carrier activity during mark periods and LOW during space periods.

The primary test is concerned with the **envelope timing**, not individual carrier cycles.

For example, a physical waveform equivalent to:

```text
38-kHz carrier activity
<--------- 1000 us --------->

LOW
<--------- 1010 us --------->

38-kHz carrier activity
<--------- 1020 us --------->

LOW
<--------- 1030 us --------->
```

must be interpreted as:

```text
1000
1010
1020
1030
```

The Python analyzer must therefore extract the envelope rather than treating every carrier cycle as a protocol duration.

Spaces are expected to be flat LOW because the carrier is applied to the active HIGH level.

---

# 11. Primary Acceptance Test

The Python/pytest test must compare:

```text
EXPECTED:
D[0], D[1], D[2], ..., D[111]

ACTUAL:
A[0], A[1], A[2], ..., A[n]
```

The test must verify:

### 11.1 Duration count

Expected:

```text
112
```

Actual must also contain:

```text
112
```

If the count differs, the test fails.

---

### 11.2 Ordering

For every index:

```text
0 <= i < 112
```

the actual duration must correspond to the expected duration at the same index.

No duration may be:

* skipped
* duplicated
* reordered
* inserted
* removed

---

### 11.3 Timing tolerance

Physical measurement will not necessarily produce exact integer equality.

The comparison must therefore use a configurable timing tolerance.

For example:

```python
abs(actual_us - expected_us) <= tolerance_us
```

The tolerance must be defined in one configurable location.

Do not scatter hard-coded tolerance values throughout the parser.

The initial tolerance should be chosen conservatively based on the actual 1-MHz RMT resolution and logic-analyzer measurement accuracy.

---

# 12. Multi-Invocation Verification

This is the most important part of the test.

The 112-duration frame produces:

```text
56 RMT symbols
```

while the custom encoder's batch size is:

```text
32 symbols
```

Therefore the test exercises the path where the encoder must preserve its state while continuing the same waveform.

The resulting physical waveform must correspond to:

```text
symbol 0
symbol 1
...
symbol 31
symbol 32
symbol 33
...
symbol 55
```

There must be no discontinuity caused by the encoder state transition.

In particular, the test must detect failures such as:

* restarting at symbol 0
* duplicating the last symbol of the first batch
* skipping the first symbol of the next batch
* advancing `symbol_index` incorrectly
* prematurely reporting `RMT_ENCODING_COMPLETE`
* transmitting fewer symbols than requested
* transmitting additional symbols
* changing the order of durations
* corrupting the second batch

The physical waveform comparison must catch these conditions.

---

# 13. Do Not Assert an Exact Encoder Call Count

The test must **not** fail merely because the RMT driver invokes the custom encoder a different number of times than expected.

For example, do not make the acceptance criterion:

```text
encoder called exactly 2 times
```

The ESP-IDF RMT driver's internal refill behavior is not the primary subject of this test.

The requirement is instead:

```text
Input:
112 durations / 56 symbols

Output:
the complete corresponding physical waveform
```

The test frame is intentionally larger than 32 symbols so that the multi-invocation path is exercised.

---

# 14. Pytest Responsibilities

The Python test suite must:

1. Generate or load the deterministic expected 112-duration sequence.
2. Obtain/read the logic-analyzer capture.
3. Parse the digital waveform.
4. Detect the carrier/envelope.
5. Extract the mark and space durations.
6. Produce the actual duration sequence.
7. Compare actual against expected.
8. Apply the configured timing tolerance.
9. Fail the test if the count, order, or timing is incorrect.
10. Produce useful diagnostics identifying the first mismatch.

The waveform parser and comparison logic should be separate components/functions.

Conceptually:

```text
load_capture()
      ↓
extract_envelope()
      ↓
compare_waveform()
      ↓
pytest assertions
```

Do not put the entire implementation into one large pytest function.

---

# 15. Failure Diagnostics

A failure must be diagnostic enough that the result can be given directly to an engineer or AI without manually inspecting the complete waveform.

For example:

```text
FAILED: AC IR transport waveform mismatch

Expected duration count: 112
Actual duration count:   112

First mismatch:
index:    32
expected: 1320 us
actual:   1310 us
error:     -10 us
```

If the number of durations differs:

```text
FAILED: duration count mismatch

Expected: 112
Actual:   111
```

The test should preferably also show a window around the first mismatch:

```text
Expected:
29: 1290
30: 1300
31: 1310
32: 1320
33: 1330
34: 1340

Actual:
29: 1291
30: 1301
31: 1311
32: 1311
33: 1331
34: 1341
```

This should make skipped/duplicated/reordered durations immediately apparent.

---

# 16. Do Not Use the Existing Debug Trace as the Test Mechanism

The supplied `ac_ir.c` contains debug structures such as:

```c
dbg[]
dbg_count
dbg_encode_calls
```

but the supplied implementation does not populate these structures inside `ac_ir_encoder_encode()`.

Therefore these debug structures are currently effectively dead/unpopulated.

The test must:

* not depend on them
* not parse them
* not use their values as evidence of correctness

They may remain in the production source for diagnostic purposes, but they are irrelevant to the automated pass/fail decision unless the production implementation is separately modified to populate them.

The physical logic-analyzer waveform is the authoritative output.

---

# 17. Synchronization / Transmission Completion

The current `ac_ir_send()` implementation waits for the RMT transmission to complete using:

```c
rmt_tx_wait_all_done(...)
```

The test firmware should therefore treat successful return from `ac_ir_send()` as the indication that the requested transmission has finished, subject to the behavior of the supplied implementation.

The existing timeout is sufficient for the proposed 112-duration test values.

If the test data is changed to substantially longer durations, the test generator must account for the existing transmission timeout.

---

# 18. Capture Handling

The pytest side must be designed around the actual logic-analyzer export format available in the development environment.

If the capture format is not supplied, do not silently invent a proprietary or unavailable format.

Instead:

* make the parser target a clearly specified supported export format, or
* isolate the capture-reader component so the actual format can be configured/adjusted.

The parser must operate on the waveform capture, not on ESP-IDF serial logs.

> **Note (deviation from this original section):** this implementation resolves the open capture-format question by not using an external logic analyzer at all — see section 9's note above. `load_capture()`'s role is filled by `pytest_ac_ir_transport.py` reading the on-device edge dump via the DUT serial connection, and `parse_edge_dump()` in `waveform_parser.py` is the isolated capture-reader component this section calls for.

---

# 19. No Haier Protocol Dependency

The test must not import, call, or depend on:

```text
haier.c
```

or any Haier-specific encoder.

The test waveform is intentionally synthetic.

This is important because the purpose is to establish an independent contract for the transport layer.

The test must remain valid even if the Haier protocol implementation is later changed completely.

---

# 20. What This Test Does NOT Test

This test must explicitly **not** attempt to determine:

* whether the Haier protocol is correct
* whether `haier.c` generated a correct frame
* whether the Haier checksum is correct
* whether the actual AC recognizes the waveform
* whether the AC changes temperature
* whether the AC changes mode
* whether the AC changes fan speed
* whether 38 kHz is the correct carrier for the appliance
* whether 33% carrier duty cycle is optimal for the appliance
* whether the complete IR protocol is semantically valid

Those are separate concerns.

This test only establishes:

```text
ac_ir_frame_t
      ↓
ac_ir RMT transport
      ↓
physical GPIO waveform
```

---

# 21. Desired Test Structure

> **Note (deviation from this original section):** the generated solution uses the standard ESP-IDF `components/<component>/test_apps/<app>/` layout instead of the originally sketched `test/esp32/` + `test/pytest/` split, per project convention. See the top-level README for the actual structure.

---

# 22. Information That Must Come From the Supplied Source

Before generating the test implementation, inspect the supplied `ac_ir.c` and `ac_ir.h` and determine:

1. Exact `ac_ir_frame_t` definition.
2. Exact type of `durations`.
3. Exact function declarations.
4. Required headers.
5. Required component dependencies.
6. Target MCU.
7. TX GPIO.
8. RMT resolution.
9. Carrier frequency.
10. Carrier duty cycle.
11. RMT memory configuration.
12. Custom encoder batch size.
13. How encoder state is maintained.
14. How transmission completion is handled.

Do not guess these values if they are available in the supplied files.

`ac_ir.h` must be supplied along with `ac_ir.c` so that the generated test uses the actual public interface and duration type.

---

# 23. Core Acceptance Criterion

The complete test can be reduced to this invariant:

```text
Given:

D[0], D[1], D[2], ... D[111]

the physical RMT output envelope must be:

D[0], D[1], D[2], ... D[111]

within the defined timing tolerance.
```

The input contains 56 symbols, exceeding the custom encoder's 32-symbol batch size.

Therefore the test specifically exercises the encoder's ability to continue one logical transmission across multiple encoder invocations.

A passing test establishes:

> **The `ac_ir` transport layer faithfully transmits a large externally supplied waveform across its multi-invocation RMT encoding path.**

It does **not** establish that the waveform is a valid Haier AC protocol frame.

That is intentionally outside the scope of this test.
