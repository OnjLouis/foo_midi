# Multi-Port Community Build: Technical Notes

This fork is based on stuerp/foo_midi 3.2.3.0 and its pinned libmidi dependency.
Original author credits and licenses remain unchanged. Version 3.2.3.3 is the
first public community preview, not an upstream release or a replacement MU
hardware emulator. The development branch retains the historical name
`yamaha-midi-ports`; the component supports Yamaha and Roland routing.

For installation and updates, see the [HTML guide](Multi-Port-Community-Build.html).

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
- Recognize the complete SC-88 type-1 `PartA 1ch.` through `PartB 16ch.`
  track-name layout when a GS reset is present and no explicit port or
  instrument/device routing metadata is present. Do not guess from track count.
- Recognize partial SC-8820 `A01-Instrument` / `B01-Instrument` layouts under
  the same conditions. Require both groups, unique part labels, and matching
  MIDI channels on every musical track. Explicit port metadata takes priority.
- Adapt XG multi-part parameter changes for parts 17-64 to the correct plugin
  instance and its local part number, including same-port receive channels.
- Copy global resets and recognized system effects to declared ports. Local
  part messages are not broadcast. Rebase variation/insertion part assignments
  and disable their assignment in other instances.
- Translate valid Roland GS alternate-group addresses from A to B or B to A,
  rebasing the local block and recalculating the DT1 checksum. Leave invalid
  checksums unchanged. Without a declared destination port, preserve the
  original message on its source for native multi-part handling.
- Apply the same SysEx policy to VST and Secret Sauce playback, timed and
  untimed. Secret Sauce no longer broadcasts every part-specific port-A SysEx
  or aliases unsupported event ports onto A.

MIDI track count is not MIDI channel count. Multiple tracks may use the same
channel; independent groups of 16 channels need port information.

## Scope and limitations

Four independent VST instances do not reproduce a single MU's shared global
effects, cross-port receive-channel reassignment, or shared polyphony. Recognized
parameter changes are adapted; proprietary bulk dumps and unknown model IDs
remain unchanged on their declared port. This does not alter voices or guarantee
every MU demo will sound identical to hardware. Port numbering retains libmidi's
existing normalization. Translation requires the destination port to be declared
by the MIDI file; missing ports are not invented from a SysEx address. Messages
whose destination is absent remain unchanged on their source port. A declared
port beyond the player's capacity remains unsupported rather than being aliased.
VST ports beyond the first four normalized ports are unsupported and ignored,
not merged into another port.
Secret Sauce retains its existing three-port capacity.

Address interpretation follows the Yamaha XG parameter-change tables and
[Roland SC-88 Pro MIDI implementation](https://cdn.roland.com/assets/media/pdf/SC-88PRO_OM.pdf).
The original MIDI file is never rewritten by playback routing.

Regression tests use a ROM-free VST probe and synthetic MIDI files. They cover
independent programs/notes, timed events and SysEx, resets, state transfer,
unsupported ports, lazy instances, and repeated startup/shutdown. Both Win32
and x64 variants pass the parser and bridge tests. Listening checks confirmed
the SC-88 demos and the corrected multi-port Yamaha percussion case. Corpus
audits checked channel routing and preserved message payloads across Yamaha
and Roland files, including conservative partial SC-8820 layouts. These tests
do not establish complete hardware parity or long-running stability on every
plugin and machine; further community listening tests remain valuable.

## Tests

The parser tests live in the libmidi submodule's `tests` directory. VST bridge
tests live in `vsthost/tests`; configure that directory with CMake, a Windows
C++17 compiler and Python 3, using an external build directory. Run both Win32
and x64 variants. No ROMs or commercial MIDI files are required.

The community build workflow uses the SDK version documented by upstream,
2025-03-07, and a pinned WTL checkout. It uploads test artifacts only; it never
creates a GitHub release or installs anything on a user's machine.
