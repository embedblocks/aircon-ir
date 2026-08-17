"""
Protocol-agnostic waveform generators and characterization matrix.

These are DATA generators.

They do not tune ac_ir.c.

The production transport configuration remains fixed while the input
waveform is varied to discover where the implementation succeeds/fails.
"""

from typing import List


def generate_uniform(
    count: int,
    start_us: int,
    step_us: int,
) -> List[int]:
    """durations[i] = start_us + step_us * i."""
    return [
        start_us + step_us * i
        for i in range(count)
    ]


def generate_leader_burst(
    leader_mark_us: int,
    leader_space_us: int,
    burst_pairs: int,
    bit_mark_us: int,
    bit_space_us: int,
) -> List[int]:
    """
    [leader_mark, leader_space,
     bit_mark, bit_space,
     bit_mark, bit_space, ...]
    """

    durations = [
        leader_mark_us,
        leader_space_us,
    ]

    for _ in range(burst_pairs):
        durations.append(bit_mark_us)
        durations.append(bit_space_us)

    return durations


def uniform_symbols(
    symbols: int,
    start_us: int = 1000,
    step_us: int = 10,
):
    """
    Convert symbol count to an even duration count.

    One RMT symbol = one mark + one space.
    """

    return {
        "count": symbols * 2,
        "start_us": start_us,
        "step_us": step_us,
    }


def make_uniform_case(
    name: str,
    symbols: int,
    why: str,
    start_us: int = 1000,
    step_us: int = 10,
):
    count = symbols * 2

    return {
        "name": name,
        "load_cmd": (
            f"LOAD UNIFORM "
            f"{count} "
            f"{start_us} "
            f"{step_us}"
        ),
        "expected": generate_uniform(
            count,
            start_us,
            step_us,
        ),
        "why": why,
    }

CHARACTERIZATION_SHAPES = [
    make_uniform_case(
        f"symbols_{symbols}",
        symbols,
        f"Boundary search: {symbols} symbols.",
    )
    for symbols in range(50, 51)
]