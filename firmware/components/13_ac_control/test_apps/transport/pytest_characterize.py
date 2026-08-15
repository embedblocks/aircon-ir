"""
Characterization test for the fixed ac_ir transport.

This test does NOT tune ac_ir.c.

It:

    1. Boots the test firmware once.
    2. LOADs a waveform.
    3. RUNs it.
    4. Captures the physical TX waveform.
    5. Compares it with the expected waveform.
    6. Records PASS/FAIL.
    7. Repeats for every waveform shape.

The purpose is to discover the operating boundary of the CURRENT
implementation.
"""

import os
import re

import pytest
from pytest_embedded_idf.dut import IdfDut

from shapes import CHARACTERIZATION_SHAPES
from waveform_parser import compare_waveform


TIMING_TOLERANCE_US = int(
    os.environ.get(
        "AC_IR_TIMING_TOLERANCE_US",
        "20",
    )
)


LOAD_RESULT_RE = re.compile(
    rb"(LOAD_OK .*|LOAD_ERR .*)"
)

DURATION_COUNT_RE = re.compile(
    rb"DURATION_COUNT=(\d+)"
)

SYMBOL_COUNT_RE = re.compile(
    rb"EXPECTED_SYMBOL_COUNT=(\d+)"
)

SEND_RESULT_RE = re.compile(
    rb"AC_IR_SEND_RESULT=(-?\d+)"
)

EDGE_COUNT_RE = re.compile(
    rb"EDGE_COUNT=(\d+)"
)

OVERFLOW_RE = re.compile(
    rb"EDGE_OVERFLOW=([01])"
)

TX_DONE_RE = re.compile(
    rb"TX_DONE_TS_US=(\d+)"
)

ACTUAL_COUNT_RE = re.compile(
    rb"ACTUAL_DURATION_COUNT=(\d+)"
)

ACTUAL_DURATIONS_RE = re.compile(
    rb"ACTUAL_DURATIONS=([0-9,]*)"
)


def run_one_shape(
    dut: IdfDut,
    shape: dict,
):
    """
    Execute one LOAD/RUN experiment.

    Returns:

        (status, detail)

    Status is one of:

        PASS
        LOAD_ERROR
        SEND_FAILED
        EDGE_OVERFLOW
        PROTOCOL_MISMATCH
        WAVEFORM_MISMATCH
    """

    # --------------------------------------------------------------
    # LOAD
    # --------------------------------------------------------------

    dut.write(
        shape["load_cmd"] + "\n"
    )

    load_match = dut.expect(
        LOAD_RESULT_RE,
        timeout=10,
    )

    load_line = load_match.group(1)

    if load_line.startswith(b"LOAD_ERR"):
        return (
            "LOAD_ERROR",
            load_line.decode(
                errors="replace"
            ),
        )

    # --------------------------------------------------------------
    # RUN
    # --------------------------------------------------------------

    dut.write("RUN\n")

    dut.expect_exact(
        "AC_IR_TRANSPORT_TEST_START",
        timeout=10,
    )

    duration_count = int(
        dut.expect(
            DURATION_COUNT_RE,
            timeout=10,
        ).group(1)
    )

    expected_count = len(
        shape["expected"]
    )

    if duration_count != expected_count:
        return (
            "PROTOCOL_MISMATCH",
            (
                f"Firmware reports "
                f"DURATION_COUNT={duration_count}, "
                f"but loaded waveform has "
                f"{expected_count} durations."
            ),
        )

    dut.expect(
        SYMBOL_COUNT_RE,
        timeout=10,
    )

    # --------------------------------------------------------------
    # Wait for send result
    # --------------------------------------------------------------

    dut.expect_exact(
    "AC_IR_TRANSPORT_TEST_TX_DONE",
    timeout=15,
    )

    send_result = int(
        dut.expect(
            SEND_RESULT_RE,
            timeout=10,
        ).group(1)
    )

    # --------------------------------------------------------------
    # Capture diagnostics
    # --------------------------------------------------------------

    edge_count = int(
        dut.expect(
            EDGE_COUNT_RE,
            timeout=10,
        ).group(1)
    )

    overflow = (
        dut.expect(
            OVERFLOW_RE,
            timeout=10,
        ).group(1)
        == b"1"
    )

    dut.expect(
        TX_DONE_RE,
        timeout=10,
    )

    actual_count = int(
        dut.expect(
            ACTUAL_COUNT_RE,
            timeout=10,
        ).group(1)
    )

    duration_match = dut.expect(
        ACTUAL_DURATIONS_RE,
        timeout=10,
    )

    raw = duration_match.group(1)

    actual_durations = (
        [
            int(value)
            for value in raw.split(b",")
        ]
        if raw
        else []
    )

    dut.expect_exact(
        "AC_IR_TRANSPORT_TEST_END",
        timeout=10,
    )

    # --------------------------------------------------------------
    # Classify send failure
    # --------------------------------------------------------------

    if send_result != 0:
        return (
            "SEND_FAILED",
            (
                f"ac_ir_send() returned "
                f"{send_result}; "
                f"EDGE_COUNT={edge_count}"
            ),
        )

    # --------------------------------------------------------------
    # Capture overflow
    # --------------------------------------------------------------

    if overflow:
        return (
            "EDGE_OVERFLOW",
            (
                f"Raw capture overflowed at "
                f"EDGE_COUNT={edge_count}. "
                f"Increase MAX_EDGES before using "
                f"this case as a transport conclusion."
            ),
        )

    # --------------------------------------------------------------
    # Validate duration output
    # --------------------------------------------------------------

    if len(actual_durations) != actual_count:
        return (
            "PROTOCOL_MISMATCH",
            (
                f"ACTUAL_DURATION_COUNT="
                f"{actual_count}, but parsed "
                f"{len(actual_durations)} values."
            ),
        )

    # --------------------------------------------------------------
    # Compare physical waveform
    # --------------------------------------------------------------

    try:

        compare_waveform(
            shape["expected"],
            actual_durations,
            TIMING_TOLERANCE_US,
        )

    except AssertionError as exc:

        return (
            "WAVEFORM_MISMATCH",
            str(exc),
        )

    return (
        "PASS",
        (
            f"{actual_count} durations, "
            f"{edge_count} raw edges"
        ),
    )


@pytest.mark.target("esp32c3")
@pytest.mark.env("generic")
def test_ac_ir_characterize_shapes(
    dut: IdfDut,
) -> None:

    # --------------------------------------------------------------
    # Firmware ready
    # --------------------------------------------------------------

    dut.expect_exact(
        "AC_IR_TEST_READY",
        timeout=30,
    )

    results = []

    # --------------------------------------------------------------
    # Execute entire characterization matrix
    # --------------------------------------------------------------

    for index, shape in enumerate(
        CHARACTERIZATION_SHAPES,
        start=1,
    ):

        print(
            f"\n[{index}/{len(CHARACTERIZATION_SHAPES)}] "
            f"{shape['name']}"
        )

        status, detail = run_one_shape(
            dut,
            shape,
        )

        results.append(
            (
                shape,
                status,
                detail,
            )
        )

        print(
            f"  RESULT: {status}"
        )

        if detail:
            print(
                f"  DETAIL: {detail}"
            )

    # --------------------------------------------------------------
    # Final characterization report
    # --------------------------------------------------------------

    print(
        "\n\n"
        "============================================================\n"
        " AC_IR TRANSPORT CHARACTERIZATION\n"
        " Fixed ac_ir.c / varying input data\n"
        "============================================================"
    )

    print(
        f"{'CASE':<34} "
        f"{'RESULT':<22} "
        f"DETAIL"
    )

    print(
        "-" * 100
    )

    for shape, status, detail in results:

        short_detail = ""

        if detail:
            short_detail = (
                detail
                .splitlines()[0]
            )

        print(
            f"{shape['name']:<34} "
            f"{status:<22} "
            f"{short_detail}"
        )

    # --------------------------------------------------------------
    # Findings
    # --------------------------------------------------------------

    failures = [
        result
        for result in results
        if result[1] != "PASS"
    ]

    print(
        "\n"
        "============================================================"
    )

    print(
        f"TOTAL CASES : {len(results)}"
    )

    print(
        f"PASSED      : "
        f"{len(results) - len(failures)}"
    )

    print(
        f"NON-PASS    : "
        f"{len(failures)}"
    )

    print(
        "============================================================"
    )

    if failures:

        print(
            "\n"
            "NON-PASS CASE DETAILS"
        )

        for shape, status, detail in failures:

            print(
                f"\n[{shape['name']}]"
            )

            print(
                f"STATUS: {status}"
            )

            print(
                f"WHY: {shape['why']}"
            )

            if detail:
                print(
                    f"DETAIL: {detail}"
                )

    # --------------------------------------------------------------
    # Characterization is allowed to find failures.
    #
    # We intentionally do NOT assert that every waveform passes.
    #
    # The purpose of this test is to map the current implementation's
    # behavior, not to pretend that known failures are successful.
    #
    # We only fail if the characterization infrastructure itself
    # could not execute any cases.
    # --------------------------------------------------------------

    assert results, (
        "No characterization cases were executed."
    )