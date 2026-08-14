"""
Parses the raw edge-timestamp dump produced by the ac_ir transport test
firmware, reconstructs the physical mark/space envelope, and compares it
against the expected duration sequence.

This module treats the edge dump as the sole source of truth about what
physically appeared on the wire. It has no knowledge of, and does not
read, ac_ir.c's internal dbg[] / dbg_encode_calls trace -- per
requirements.md section 16, that trace is unpopulated in the supplied
implementation and must not be used as evidence of correctness.

Pipeline (requirements.md section 14):
    parse_edge_dump()  -> raw (level, timestamp) edges
    extract_envelope() -> reconstructed mark/space duration sequence
    compare_waveform() -> pass/fail + diagnostics
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List, Tuple

# A gap between consecutive recorded edges longer than this is treated as
# a "space" (flat LOW, no carrier) rather than a normal 38 kHz carrier
# half-cycle gap (~13us nominal, comfortably under 100us even accounting
# for duty-cycle asymmetry and ISR timestamp jitter). It must stay well
# below the shortest expected space duration in the test data (1000us).
CARRIER_GAP_THRESHOLD_US = 100

_EDGE_LINE_RE = re.compile(r'^([01]),(\d+)$', re.MULTILINE)
_EDGE_COUNT_RE = re.compile(r'EDGE_COUNT=(\d+)')
_OVERFLOW_RE = re.compile(r'EDGE_OVERFLOW=([01])')
_TX_DONE_RE = re.compile(r'TX_DONE_TS_US=(\d+)')


@dataclass
class EdgeDump:
    edge_count: int
    overflow: bool
    tx_done_ts_us: int
    edges: List[Tuple[int, int]]  # (level, timestamp_us), time-ordered


def parse_edge_dump(raw_text: str) -> EdgeDump:
    """Parse the text block between AC_IR_EDGE_DUMP_START/END."""
    count_match = _EDGE_COUNT_RE.search(raw_text)
    overflow_match = _OVERFLOW_RE.search(raw_text)
    tx_done_match = _TX_DONE_RE.search(raw_text)

    if not (count_match and overflow_match and tx_done_match):
        raise ValueError(
            'edge dump missing EDGE_COUNT / EDGE_OVERFLOW / TX_DONE_TS_US header; '
            'got:\n' + raw_text[:500]
        )

    edge_count = int(count_match.group(1))
    overflow = overflow_match.group(1) == '1'
    tx_done_ts_us = int(tx_done_match.group(1))

    edges = [(int(level), int(ts)) for level, ts in _EDGE_LINE_RE.findall(raw_text)]

    if len(edges) != edge_count:
        raise ValueError(
            f'EDGE_COUNT header says {edge_count} but parsed {len(edges)} edge lines '
            '(serial capture likely truncated or corrupted)'
        )

    return EdgeDump(edge_count=edge_count, overflow=overflow,
                     tx_done_ts_us=tx_done_ts_us, edges=edges)


def extract_envelope(dump: EdgeDump) -> List[int]:
    """
    Reconstruct the mark/space duration sequence from raw edge timestamps.

    The carrier is only applied during "mark" (level=1) periods (see
    requirements.md section 10), so a mark appears as a burst of rapid
    toggles at ~38kHz, and a "space" appears as a gap with no edges at
    all (sustained LOW). A gap longer than CARRIER_GAP_THRESHOLD_US
    between consecutive edges marks a mark->space or space->mark
    transition.

    The final space has no trailing edge to bound it -- the waveform
    just ends once the last duration elapses. It is instead bounded by
    dump.tx_done_ts_us, the timestamp captured immediately after
    ac_ir_send() returned. This introduces a small, one-sided
    measurement uncertainty on the last duration only (task-wake
    latency after the RMT "done" event), which the caller's timing
    tolerance must absorb.
    """
    edges = dump.edges
    if not edges:
        return []

    if edges[0][0] != 1:
        raise ValueError(
            f'expected first captured edge to be rising (mark start), '
            f'got level={edges[0][0]} -- check wiring/idle level'
        )

    durations: List[int] = []
    mark_start_ts = edges[0][1]
    n = len(edges)

    for i in range(n):
        level, ts = edges[i]
        is_last_edge = (i == n - 1)
        next_gap = (edges[i + 1][1] - ts) if not is_last_edge else None
        is_boundary = is_last_edge or (next_gap is not None and next_gap > CARRIER_GAP_THRESHOLD_US)

        if is_boundary and level == 0:
            mark_end_ts = ts
            durations.append(mark_end_ts - mark_start_ts)

            if is_last_edge:
                durations.append(dump.tx_done_ts_us - mark_end_ts)
            else:
                next_mark_start_ts = edges[i + 1][1]
                durations.append(next_mark_start_ts - mark_end_ts)
                mark_start_ts = next_mark_start_ts

    return durations


@dataclass
class WaveformMismatch:
    index: int
    expected_us: int
    actual_us: int

    @property
    def error_us(self) -> int:
        return self.actual_us - self.expected_us


def compare_waveform(expected: List[int], actual: List[int], tolerance_us: int) -> None:
    """
    Compare expected vs actual duration sequences.

    Raises AssertionError with a diagnostic message (requirements.md
    section 15) on a count mismatch or any out-of-tolerance value.
    Returns None on success.
    """
    if len(expected) != len(actual):
        raise AssertionError(
            'FAILED: duration count mismatch\n\n'
            f'Expected: {len(expected)}\n'
            f'Actual:   {len(actual)}'
        )

    mismatches = [
        WaveformMismatch(i, e, a)
        for i, (e, a) in enumerate(zip(expected, actual))
        if abs(a - e) > tolerance_us
    ]

    if not mismatches:
        return

    first = mismatches[0]
    window_lo = max(0, first.index - 3)
    window_hi = min(len(expected), first.index + 4)

    expected_window = '\n'.join(f'{i}: {expected[i]}' for i in range(window_lo, window_hi))
    actual_window = '\n'.join(f'{i}: {actual[i]}' for i in range(window_lo, window_hi))

    raise AssertionError(
        'FAILED: AC IR transport waveform mismatch\n\n'
        f'Expected duration count: {len(expected)}\n'
        f'Actual duration count:   {len(actual)}\n\n'
        f'Total mismatches (tolerance={tolerance_us}us): {len(mismatches)}\n\n'
        'First mismatch:\n'
        f'index:    {first.index}\n'
        f'expected: {first.expected_us} us\n'
        f'actual:   {first.actual_us} us\n'
        f'error:    {first.error_us:+d} us\n\n'
        f'Expected:\n{expected_window}\n\n'
        f'Actual:\n{actual_window}'
    )
