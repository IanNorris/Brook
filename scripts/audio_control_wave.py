"""Check the order of sustained amplitude levels in the volume test capture."""
from collections import Counter
from itertools import groupby


def volume_phase_runs(samples, block_samples=8820, minimum_blocks=3):
    """Return sustained dominant amplitudes; ignore transitions and short gaps.

    The acceptance fixture uses interleaved 44.1 kHz stereo and constant
    magnitude square waves. Each default block is therefore 100 ms. A block
    needs 80% agreement to avoid counting transition blocks as a phase.
    """
    runs = []
    previous = None
    count = 0
    for start in range(0, len(samples) - block_samples + 1, block_samples):
        block = samples[start:start + block_samples]
        amplitude, frequency = Counter(abs(sample) for sample in block).most_common(1)[0]
        value = amplitude if frequency * 5 >= block_samples * 4 else None
        if value != previous:
            if previous is not None and count >= minimum_blocks:
                runs.append(previous)
            previous, count = value, 0
        count += 1
    if previous is not None and count >= minimum_blocks:
        runs.append(previous)
    return runs


def has_ordered_phases(runs, expected=(0, 2000, 4000, 8000, 0, 2000)):
    iterator = iter(runs)
    return all(any(actual == wanted for actual in iterator) for wanted in expected)


def steady_tone_frames(samples, peak=8000, half_period=50):
    """Count the longest exact stereo run of alternating half-periods."""
    frames = zip(samples[::2], samples[1::2])
    levels = (left if left == right and abs(left) == peak else 0 for left, right in frames)
    longest = run = previous = 0
    for level, values in groupby(levels):
        length = sum(1 for _ in values)
        if level and length == half_period:
            run = run + length if level == -previous else length
            longest = max(longest, run)
            previous = level
        else:
            run = previous = 0
    return longest
