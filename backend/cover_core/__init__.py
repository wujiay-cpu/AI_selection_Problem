"""cover_core package init.

Avoid noisy import warnings here. The runtime import routing is handled in
`backend.algorithm` with explicit capability checks.
"""

try:
    from .cover_core_ext import greedy_selection, greedy_fast_selection, backtracking_selection, beam_search_selection
except Exception:
    # Keep package import silent; caller will decide fallback strategy.
    pass

__all__ = [
    "greedy_selection",
    "greedy_fast_selection",
    "backtracking_selection",
    "beam_search_selection",
]
