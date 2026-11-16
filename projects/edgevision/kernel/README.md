# UVC trace patch

This directory contains a small driver patch against the frozen NVIDIA L4T
R36.4.3 `drivers/media/usb/uvc` sources. Original kernel files retain their own
SPDX/copyright notices; the new trace header is GPL-2.0-or-later. The repository's
NVIDIA MIT license does not replace these driver licenses.

The exact official archive, unmodified-source hashes, target configuration and
source call chain are documented in [M3](../docs/m3-kernel.md).
[M4](../docs/m4-uvc-trace.md) links actual build, loaded-module identity, raw
trace, parser, USB recovery and stock rollback evidence.

`make_uvc_trace_patch.py` regenerates the patch from the frozen pristine tree and
this `uvc_trace.h`. `build_uvc_trace.py` applies it with zero fuzz in a new owned
artifact directory and checks imported symbol CRCs. It does not install modules.
Runtime preparation and verification are separate `verify_uvc_trace.py` phases.
