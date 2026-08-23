"""BLE bridge — projects the dashboard's state onto M5Stack panels.

The bridge is a *client* of the dashboard, not a stage inside it: it subscribes to
``ws://127.0.0.1:8420/ws`` and receives exactly the frames the browser gets, then
answers device polls with per-screen JSON small enough for one BLE write
(architecture §1.2).

**Every computation lives here.** The device renders JSON and does nothing else — no
statistics, no history, no timers, no state between frames. That is a testability
decision before it is an architectural one: it moves logic out of the only place that
can be checked solely by eye and into the place ``pytest`` already reaches
(device-frontends-vision.md §3.1).

Unlike ``tracker/`` and ``hooks/``, this package may use third-party imports. It is a
separate process started deliberately, exactly like ``dashboard/``, and is never on the
pipeline's critical path. Nothing imports it: it is a leaf, so the dashboard keeps
starting on a machine with no Bluetooth and no ``bleak`` installed.
"""
