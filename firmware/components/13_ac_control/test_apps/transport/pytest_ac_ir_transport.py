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
import os
import re

import pytest
from pytest_embedded_idf.dut import IdfDut

from expected_waveform import generate_expected_durations
from waveform_parser import parse_edge_dump, extract_envelope, compare_waveform

TIMING_TOLERANCE_US = int(os.environ.get('AC_IR_TIMING_TOLERANCE_US', '20'))

_EDGE_DUMP_BLOCK_RE = re.compile(rb'(.*?)AC_IR_EDGE_DUMP_END', re.DOTALL)


@pytest.mark.target('esp32c3')
@pytest.mark.env('generic')
def test_ac_ir_transport_waveform(dut: IdfDut) -> None:
    dut.expect_exact('AC_IR_TRANSPORT_TEST_START', timeout=30)

    duration_count_match = dut.expect(re.compile(rb'DURATION_COUNT=(\d+)'), timeout=10)
    assert int(duration_count_match.group(1)) == 112, 'unexpected DURATION_COUNT from firmware'

    symbol_count_match = dut.expect(re.compile(rb'EXPECTED_SYMBOL_COUNT=(\d+)'), timeout=10)
    assert int(symbol_count_match.group(1)) == 56, 'unexpected EXPECTED_SYMBOL_COUNT from firmware'

    dut.expect_exact('AC_IR_TRANSPORT_TEST_TX_DONE', timeout=10)

    # Read the entire edge-dump block in one go rather than line-by-line;
    # this can be several thousand lines for the primary test frame.
    dut.expect_exact('AC_IR_EDGE_DUMP_START', timeout=10)
    block_match = dut.expect(_EDGE_DUMP_BLOCK_RE, timeout=60)
    raw_block = block_match.group(1).decode('utf-8', errors='replace')

    dut.expect_exact('AC_IR_TRANSPORT_TEST_END', timeout=10)

    dump = parse_edge_dump(raw_block)

    assert not dump.overflow, (
        'edge capture buffer overflowed on-device -- the test harness '
        'itself lost data (see MAX_EDGES in main/test_app_main.c); this capture '
        'cannot be trusted and must not be used to judge ac_ir.c'
    )

    actual_durations = extract_envelope(dump)
    expected_durations = generate_expected_durations()

    compare_waveform(expected_durations, actual_durations, TIMING_TOLERANCE_US)
