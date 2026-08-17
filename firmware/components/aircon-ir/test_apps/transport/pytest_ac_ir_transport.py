"""
Hardware verification test for the ac_ir RMT transport layer.

Verifies (requirements.md section 1):
    Given an ac_ir_frame_t containing a known sequence of durations, the
    ac_ir RMT transport layer physically transmits that same duration
    sequence, in the same order, without losing, duplicating,
    reordering, or otherwise corrupting durations across multiple RMT
    encoder invocations.

Explicitly does NOT test the Haier protocol, frame encoding, or
checksum, and does not use haier.c (requirements.md section 20).

Physical setup required:
    Jumper wire from the ac_ir TX GPIO (GPIO10, in ac_ir.c) to the RX
    capture GPIO configured via CONFIG_AC_IR_TEST_RX_GPIO (default
    GPIO3 -- see sdkconfig.defaults / menuconfig "AC IR Transport Test
    Configuration"). The RX pin is a plain interrupt-capable GPIO
    input, not an RMT channel, so the measurement path shares no code
    with the ac_ir RMT TX pipeline under test.

Run:
    cd components/ac_ir/test_apps/transport
    idf.py set-target esp32c3
    idf.py build flash
    pytest pytest_ac_ir_transport.py --target esp32c3 --port COM<N> \\
        --skip-autoflash=y --embedded-services esp,idf -s -v

Timing tolerance is configurable via the AC_IR_TIMING_TOLERANCE_US
env var (requirements.md section 11.3); default 20us.
"""
"""
Hardware verification test for the ac_ir RMT transport layer.

The ESP test firmware:
    1. Transmits the test waveform through the real ac_ir.c.
    2. Captures the physical TX GPIO with an independent GPIO edge ISR.
    3. Reconstructs the mark/space envelope on the ESP.
    4. Prints the resulting duration sequence.

This Python test only receives that compact reconstructed sequence and
compares it with the expected waveform.
"""

import os
import re

import pytest
from pytest_embedded_idf.dut import IdfDut

from expected_waveform import generate_expected_durations
from waveform_parser import compare_waveform


TIMING_TOLERANCE_US = int(
    os.environ.get("AC_IR_TIMING_TOLERANCE_US", "20")
)


@pytest.mark.target("esp32c3")
@pytest.mark.env("generic")
def test_ac_ir_transport_waveform(dut: IdfDut) -> None:

    # ------------------------------------------------------------------
    # Test start / configuration
    # ------------------------------------------------------------------

    dut.expect_exact(
        "AC_IR_TRANSPORT_TEST_START",
        timeout=30,
    )

    duration_count_match = dut.expect(
        re.compile(rb"DURATION_COUNT=(\d+)"),
        timeout=10,
    )

    expected_duration_count = int(
        duration_count_match.group(1)
    )

    assert expected_duration_count == 112, (
        "unexpected DURATION_COUNT from firmware: "
        f"{expected_duration_count}"
    )

    symbol_count_match = dut.expect(
        re.compile(rb"EXPECTED_SYMBOL_COUNT=(\d+)"),
        timeout=10,
    )

    expected_symbol_count = int(
        symbol_count_match.group(1)
    )

    assert expected_symbol_count == 56, (
        "unexpected EXPECTED_SYMBOL_COUNT from firmware: "
        f"{expected_symbol_count}"
    )

    # ------------------------------------------------------------------
    # Wait for physical TX to finish
    # ------------------------------------------------------------------

    dut.expect_exact(
        "AC_IR_TRANSPORT_TEST_TX_DONE",
        timeout=10,
    )

    # ------------------------------------------------------------------
    # Check capture integrity
    # ------------------------------------------------------------------

    edge_count_match = dut.expect(
        re.compile(rb"EDGE_COUNT=(\d+)"),
        timeout=10,
    )

    edge_count = int(edge_count_match.group(1))

    overflow_match = dut.expect(
        re.compile(rb"EDGE_OVERFLOW=([01])"),
        timeout=10,
    )

    edge_overflow = overflow_match.group(1) == b"1"

    assert not edge_overflow, (
        "edge capture buffer overflowed on-device; "
        f"EDGE_COUNT={edge_count}. "
        "The physical capture cannot be trusted."
    )

    # TX_DONE_TS_US is diagnostic information. Read it so the serial
    # protocol stays explicit, although Python does not need it.
    dut.expect(
        re.compile(rb"TX_DONE_TS_US=(\d+)"),
        timeout=10,
    )

    # ------------------------------------------------------------------
    # Receive reconstructed waveform
    # ------------------------------------------------------------------

    actual_count_match = dut.expect(
        re.compile(rb"ACTUAL_DURATION_COUNT=(\d+)"),
        timeout=10,
    )

    actual_count = int(
        actual_count_match.group(1)
    )

    durations_match = dut.expect(
        re.compile(rb"ACTUAL_DURATIONS=([0-9,]+)"),
        timeout=10,
    )

    actual_durations = [
        int(value)
        for value in durations_match.group(1).split(b",")
    ]

    assert len(actual_durations) == actual_count, (
        "firmware duration count does not match "
        f"the number of printed durations: "
        f"count={actual_count}, "
        f"parsed={len(actual_durations)}"
    )

    # ------------------------------------------------------------------
    # End of firmware test
    # ------------------------------------------------------------------

    dut.expect_exact(
        "AC_IR_TRANSPORT_TEST_END",
        timeout=10,
    )

    # ------------------------------------------------------------------
    # Compare against expected waveform
    # ------------------------------------------------------------------

    expected_durations = generate_expected_durations()

    assert actual_count == expected_duration_count, (
        "FAILED: duration count mismatch\n\n"
        f"Expected: {expected_duration_count}\n"
        f"Actual:   {actual_count}"
    )

    compare_waveform(
        expected_durations,
        actual_durations,
        TIMING_TOLERANCE_US,
    )