"""RoboFace server.

The tier that owns every decision: the wire contract (`protocol`), one connection state
machine per device (`router`), and -- from v0.2 -- the orchestrator and the provider
seams. The device renders what this package sends and reports what it senses; no
intelligence lives on the device (ARCHITECTURE.md §Overview).

Importable as `roboface_server` because the root pyproject puts `server/` on the path
via pytest's `pythonpath`; the package is never installed.
"""

__all__ = ["__version__"]

#: The running server version. Kept in step with the release tag `vA.B.C` by
#: `release-version`, and with the `version=` string the FastAPI app reports.
__version__ = "0.1.0"
