"""Nonblocking copied-profile ownership, explicitly limited to Windows."""

import sys


def lock_profile(lock) -> None:
    if sys.platform == "win32":
        import msvcrt

        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        raise ValueError(
            "The experimental gameplay launcher currently supports Windows only"
        )
