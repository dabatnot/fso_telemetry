#!/usr/bin/env python3
"""Stable command-line entry point for the independent FSTL client."""

from fstl_client_core import *  # noqa: F401,F403
from fstl_client_core import _record_identity  # noqa: F401


if __name__ == "__main__":
    raise SystemExit(main())
