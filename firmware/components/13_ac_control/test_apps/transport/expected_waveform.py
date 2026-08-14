"""
Deterministic expected duration sequence for the ac_ir transport test.

MUST stay in sync with build_test_durations() in
main/test_app_main.c (START_US / STEP_US / DURATION_COUNT). Both sides
independently implement the same simple arithmetic sequence rather than
sharing a generated file, since they run in different toolchains -- if
you change one, change the other.

See requirements.md section 6 for why the sequence is monotonically
increasing and deterministic rather than random: every duration must be
identifiable by its position, so a skipped/duplicated/reordered value is
immediately visible in a diff rather than hidden by coincidental repeats.
"""

DURATION_COUNT = 112
START_US = 1000
STEP_US = 10


def generate_expected_durations():
    """
    durations[0] = mark, durations[1] = space, durations[2] = mark, ...

    112 values -> 56 RMT symbols, which exceeds the ac_ir custom
    encoder's 32-symbol batch size (see requirements.md section 5),
    so this frame necessarily exercises the encoder's multi-invocation
    continuation path.
    """
    return [START_US + STEP_US * i for i in range(DURATION_COUNT)]
