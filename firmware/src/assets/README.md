# Generated face art

`skin_assets.bin` and `skin_assets.h` are **generated** — do not edit them.

```bash
.venv/bin/python tools/skin_assets.py
```

The source PNGs live in [`specification/features/skin-assets/assets/`](../../../specification/features/skin-assets/assets),
drawn to [SKIN_ASSETS.md](../../../specification/features/SKIN_ASSETS.md). The converter decodes them
on the workstation and emits a flat binary, so the firmware links no PNG decoder and copies nothing
to RAM: a body is 150 KB, it never changes, and flash is memory-mapped.
