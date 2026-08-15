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

    # --------------------------------------------------------------
    # Encoder boundary characterization
    # --------------------------------------------------------------

    make_uniform_case(
        "symbols_1",
        1,
        "Smallest possible one-symbol transmission.",
    ),

    make_uniform_case(
        "symbols_2",
        2,
        "Small multi-symbol transmission.",
    ),

    make_uniform_case(
        "symbols_31",
        31,
        "One symbol below the 32-symbol encoder chunk.",
    ),

    make_uniform_case(
        "symbols_32",
        32,
        "Exactly one 32-symbol encoder chunk.",
    ),

    make_uniform_case(
        "symbols_33",
        33,
        "First case requiring a second encoder invocation.",
    ),

    make_uniform_case(
        "symbols_63",
        63,
        "One symbol below 64, i.e. below two full 32-symbol chunks.",
    ),

    make_uniform_case(
        "symbols_64",
        64,
        "Exactly two full 32-symbol chunks.",
    ),

    make_uniform_case(
        "symbols_65",
        65,
        "One symbol beyond two full chunks.",
    ),

    make_uniform_case(
        "symbols_95",
        95,
        "One symbol below three 32-symbol chunks.",
    ),

    make_uniform_case(
        "symbols_96",
        96,
        "Exactly three full 32-symbol chunks.",
    ),

    make_uniform_case(
        "symbols_97",
        97,
        "One symbol beyond three full chunks.",
    ),

    # --------------------------------------------------------------
    # Existing Haier-like case
    # --------------------------------------------------------------

    {
        "name": "baseline_uniform_112",
        "load_cmd": "LOAD UNIFORM 112 1000 10",
        "expected": generate_uniform(
            112,
            1000,
            10,
        ),
        "why": (
            "Original 112-duration diagnostic case. "
            "This is the currently failing baseline."
        ),
    },

    # --------------------------------------------------------------
    # Odd duration counts
    # --------------------------------------------------------------

    {
        "name": "odd_111_durations",
        "load_cmd": "LOAD UNIFORM 111 1000 10",
        "expected": generate_uniform(
            111,
            1000,
            10,
        ),
        "why": (
            "Odd duration count exercises the final "
            "single-duration symbol/padding path."
        ),
    },

    {
        "name": "odd_113_durations",
        "load_cmd": "LOAD UNIFORM 113 1000 10",
        "expected": generate_uniform(
            113,
            1000,
            10,
        ),
        "why": (
            "Odd duration count immediately above the "
            "original 112-duration case."
        ),
    },

    # --------------------------------------------------------------
    # Larger packets
    # --------------------------------------------------------------

    {
        "name": "large_uniform_256",
        "load_cmd": "LOAD UNIFORM 256 500 2",
        "expected": generate_uniform(
            256,
            500,
            2,
        ),
        "why": (
            "256 durations / 128 symbols. "
            "Tests many encoder continuations."
        ),
    },

    {
        "name": "large_uniform_400",
        "load_cmd": "LOAD UNIFORM 400 100 2",
        "expected": generate_uniform(
            400,
            100,
            2,
        ),
        "why": (
            "400 durations / 200 symbols. "
            "Tests long multi-invocation transmissions."
        ),
    },

    # --------------------------------------------------------------
    # Realistic mixed timing
    # --------------------------------------------------------------

    {
        "name": "leader_burst_moderate",
        "load_cmd": (
            "LOAD LEADER_BURST "
            "4500 4500 48 560 1690"
        ),
        "expected": generate_leader_burst(
            4500,
            4500,
            48,
            560,
            1690,
        ),
        "why": (
            "Long leader followed by short mark/space pairs. "
            "Tests mixed timing within one transmission."
        ),
    },

    {
        "name": "leader_burst_large",
        "load_cmd": (
            "LOAD LEADER_BURST "
            "4500 4500 200 560 1690"
        ),
        "expected": generate_leader_burst(
            4500,
            4500,
            200,
            560,
            1690,
        ),
        "why": (
            "Large mixed-timing packet. "
            "Tests whether behavior changes with frame size."
        ),
    },
]