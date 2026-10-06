# Yamaha MIDI ports candidate

This fork is based on stuerp/foo_midi 3.2.3.0 and its pinned libmidi dependency.
Original author credits and licenses remain unchanged. This branch is a test
candidate, not an upstream release or a replacement MU hardware emulator.

## Changes

- Recognize Yamaha sequencer-specific port metadata `FF 7F 04 43 00 01 pp` as
  well as standard MIDI Port metadata `FF 21 01 pp`.
- Preserve the metadata when writing an SMF, including later port changes.
- Do not let instrument/device name labels override an explicit track port.
- Count up to 64 independent port/channel pairs.
- Route four VST MIDI ports to independent plugin instances. Create additional
  instances only when their ports receive data; do not fold unsupported ports
  onto port D.
- Release queued SysEx data on port A as well as other ports.

MIDI track count is not MIDI channel count. Multiple tracks may use the same
channel; independent groups of 16 channels need port information.

## Scope and limitations

Four independent VST instances do not reproduce a single MU's shared global
effects or automatically translate MU part-address SysEx for parts 17-64. This
patch passes events to their identified port unchanged; it does not introduce
synth-specific remapping, alter voices, or guarantee every MU demo will sound
identical to hardware. Port numbering retains libmidi's existing normalization.
VST ports beyond the first four normalized ports are unsupported and ignored,
not merged into another port.

Regression tests use a ROM-free VST probe and synthetic MIDI files. They cover
independent programs/notes, timed events and SysEx, resets, state transfer,
unsupported ports, lazy instances, and repeated startup/shutdown. Listening
tests with real plugins remain necessary before recommending a release.

## Tests

The parser tests live in the libmidi submodule's `tests` directory. VST bridge
tests live in `vsthost/tests`; configure that directory with CMake, a Windows
C++17 compiler and Python 3, using an external build directory. Run both Win32
and x64 variants. No ROMs or commercial MIDI files are required.

The candidate build workflow uses the SDK version documented by upstream,
2025-03-07, and a pinned WTL checkout. It uploads test artifacts only; it never
creates a GitHub release or installs anything on a user's machine.
