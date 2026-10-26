# OhMyDisplay

[![CI](https://github.com/kernullist/OhMyDisplay/actions/workflows/ci.yml/badge.svg)](https://github.com/kernullist/OhMyDisplay/actions/workflows/ci.yml)

Windows tray utility that solves two chronic display/power annoyances with one tool:

1. **Layout Keeper** - auto-saves window layouts per display topology and restores them
   after a monitor sleep/input-switch collapses everything onto one screen.
2. **Sleep Doctor** - collects fragmented `powercfg` / event-log power signals into one
   plain-language report with one-click safe fixes.

Implements the v0 (MVP) scope of [`OhMyDisplay-design.md`](OhMyDisplay-design.md).

## Status

v0 MVP + initial v1 features, building and running (current release: v0.4.0). Single
portable exe, no .NET / external runtime. Zero `/W4 /permissive-` warnings; 104 test checks
pass.

| Component | State |
|---|---|
| Core (event bus, CCD topology + identity, power notify, JSON store, atomic write, logger) | done |
| Layout Keeper (identity, adaptive sliding debounce, capture, restore + verify/retry, labels, ghost detect) | done |
| Window coverage (taskbar rules, File Explorer, `WS_EX_APPWINDOW`, other virtual desktops, snapped windows, identical-title tie-break) | done (v0.4) |
| Sleep Doctor (powercfg collectors + locale-robust parsers, event-log correlator, rule engine, HTML/text report) | done |
| Sleep architecture + SleepStudy/DRIPS (Modern Standby detection, official report via helper, graceful S3 degrade) | done (v1) |
| Embedded report window (WebView2 when available, native text fallback) | done (v1) |
| Tray shell (icon, menu, balloons, global hotkeys, monitor label editor) | done |
| Elevated helper (`requireAdministrator`, named-pipe IPC, safe powercfg actions, admin collection) | done |
| Test harness (debounce, JSON, signature, collision tie-break, coord round-trip, match scoring, IPC framing, live window filter + restore round-trip, live doctor) | done |

## Build

Requires Visual Studio 2022 Build Tools (MSVC v143, C++20). CMake + Ninja are bundled
with the toolchain.

```powershell
pwsh -File scripts\fetch-webview2.ps1          # optional: enables the embedded WebView2 report
pwsh -File build.ps1 -Config Release           # build everything
pwsh -File build.ps1 -Config Debug -Target omd_tests
.\build\omd_tests.exe                           # run the test harness
```

The WebView2 SDK is an optional build-time dependency restored by `fetch-webview2.ps1` into
`third_party\webview2\` (not committed). When absent, CMake skips it and the Sleep Doctor
report falls back to a native text view; everything else builds unchanged.

Artifacts land in `build\`:

- `OhMyDisplay.exe` - main tray app (asInvoker, PerMonitor-V2 DPI, ~350 KB with WebView2)
- `OhMyDisplay.Elevated.exe` - elevation helper (requireAdministrator)
- `omd_tests.exe` - test harness

### Releases

Pushing a `v*` tag (e.g. `git tag v0.4.0 && git push origin v0.4.0`) runs the release
workflow: it builds Release, runs the tests, and publishes a GitHub Release with the two
exes plus a zipped bundle. Release binaries are currently unsigned (Authenticode signing is
a planned hardening item, so SmartScreen will warn on first run).

## Usage

Run `OhMyDisplay.exe`. It lives in the tray. Right-click the icon for:

- **Save / Restore layout now** (also `Ctrl+Alt+S` / `Ctrl+Alt+R`)
- **Sleep Doctor report** - embedded WebView2 window (native text if the runtime is missing)
- **Sleep Doctor report (full, admin)** - additionally collects admin-only sections
  (`/requests`, `/waketimers`) through the elevated helper over named-pipe IPC; prompts
  for consent once via UAC. Wake-on-LAN findings get a one-click **Disable wake** button.
- **Monitor labels** - name each monitor; required to disambiguate identical models
- **Detect ghost monitors** - lists phantom display entries the system still remembers

Layouts are saved automatically on first sight of a topology, right before the displays
turn off or the PC sleeps, and whenever you click **Save layout now**. Restore runs only
when a known topology looks **collapsed** (a substantial share of windows have fled to the
wrong monitor, or any window when waking from a pre-sleep snapshot), not on every
`WM_DISPLAYCHANGE`. Stable-topology events only add windows opened since the snapshot
and leave existing entries alone, so a game going fullscreen cannot overwrite them. A
second launch of the exe activates the already-running copy.

Data lives in `%LOCALAPPDATA%\OhMyDisplay\` (`layouts.json`, `settings.json`, `logs\`,
`report.html`). Nothing is sent anywhere.

## Architecture

```
src/
  core/     event_bus, message_window, power_notify, display_topology (CCD identity),
            event_log (wevtapi), store (atomic JSON), json, settings, process_util,
            paths, logger, common
  keeper/   window_capture, monitor_labels, debounce, layout_store, restore_engine,
            ghost_monitor, keeper (orchestrator)
  doctor/   powercfg_collector, rules_engine, report, doctor (orchestrator)
  shell/    tray, hotkeys, label_ui
  app/      main.cpp, manifest, resources
  elevated/ helper_main.cpp, requireAdministrator manifest
tests/      test_main.cpp
```

Core only collects/normalizes/publishes signals. Keeper and Doctor subscribe to the event
bus and react independently; they do not depend on each other.

### Notable engineering decisions

- **Hidden top-level window, not `HWND_MESSAGE`.** Message-only windows do not receive
  broadcast messages (`WM_DISPLAYCHANGE`, `WM_POWERBROADCAST`). The hub is a normal
  top-level window that is simply never shown. (Deviation from the design's wording; see
  `src/core/message_window.h`.)
- **Monitor identity** = `hash(EDID mfg + product + monitor device path)`. The device path
  embeds the connector UID, which separates identical models on different ports. Genuine
  collisions get a deterministic connector discriminator and prompt the user to label.
- **Topology signature ignores coordinates** - same monitor set = same topology, so a
  post-resume collapse still matches and restores.
- **Adaptive sliding debounce**: `clamp(base + perMonitor*count, min, max)`; a new
  topology change inside the window slides the deadline so restore fires only after
  monitors stop coming back.
- **Collapse-aware restore**: after debounce, current window monitors are compared to the
  snapshot. Restore only if at least half the matched windows sit on the wrong display.
  Otherwise existing entries are left alone (a stray `WM_DISPLAYCHANGE` from a game
  fullscreen or DPI tweak must not yank windows *or* persist the transient layout);
  windows with no entry yet are appended so newly opened apps become restorable.
- **Pre-sleep snapshot**: on display-off (or suspend while the displays are on) the
  current topology's snapshot is replaced with the live layout, which also prunes closed
  windows. The first evaluation of that topology after wake restores even a partial
  collapse, since nothing but the wake could have moved those windows. Use **Save layout
  now** to pin a rearrangement immediately.
- **Which windows are kept** follows the taskbar rules, not an exe list: visible,
  uncloaked, non-empty, and either the alt-tab owner-walk winner or `WS_EX_APPWINDOW`
  (which forces taskbar presence even for tool/owned windows). Tool and no-activate
  windows without `WS_EX_APPWINDOW` are skipped, as are suspended UWP frames. Windows
  on another virtual desktop are shell-cloaked; they are kept when
  `IVirtualDesktopManager` places them on a real, non-current desktop. File Explorer
  windows are included (older builds excluded `explorer.exe`; that persisted default is
  migrated away).
- **Snapped windows**: `rcNormalPosition` holds the pre-snap rect, so an arranged
  window is captured from its live rect and restored with an explicit move.
- **Incomplete topologies are not saved**: while Windows is re-enumerating displays a
  monitor can report no device path, which would hash to a bogus identity. The keeper
  retries a few times before accepting such a topology, and the adaptive debounce is
  sized on the last settled monitor count so a 3 -> 1 -> 3 wake dip does not fire early.
- **Restore matching** is a global highest-score assignment (HWND > title > class > exe)
  so a weak untitled window cannot steal a better match. Windows that tie on every field (seven Sticky Notes with one title) are paired by nearest current position on the same monitor, then by capture order, so they no longer shuffle. Verification is one shared 200ms
  settle, then retries only the windows that missed.
- **Coordinate model**: window rects normalized to the owning monitor work area (survives
  resolution changes); `WINDOWPLACEMENT` workspace<->screen offset handled via the primary
  work area; min/max/normal restored in one `SetWindowPlacement`.
- **Locale-robust Sleep Doctor**: `powercfg` output is localized on non-English Windows, so
  admin-required is detected via **exit code** (not text), and the wake reason falls back to
  the **event log** (whose XML field names are not localized) when `/lastwake` cannot be
  parsed. Source stays ASCII-only.
- **Elevated helper IPC over a named pipe**: the non-elevated main is the pipe *server*; the
  elevated helper is the *client* (a higher-integrity client may open a lower-integrity
  server pipe of the same user). Messages are length-prefixed UTF-8 JSON. Both ends verify
  the peer's process image path; the pipe name is per-launch unique. The helper still
  supports `collect`/`devicedisablewake` argv verbs as a legacy fallback. (Authenticode
  signature verification of the peer is a hardening item.)
- **WebView2 with a graceful fallback**: the embedded report window uses WebView2 (static
  loader, so no extra DLL to ship) when the SDK is compiled in and the Evergreen runtime is
  present, and a read-only native text view otherwise. The window runs on its own STA
  thread + message loop so worker threads can fire-and-forget. Build auto-detects the SDK
  under `third_party/webview2/`.
- **Modern Standby detection is registry-based** (`Control\Power\CsEnabled`), not parsed
  from the localized `/availablesleepstates` text. SleepStudy is only collected on Modern
  Standby systems; legacy S3 machines degrade gracefully. The full report is generated by
  `powercfg /sleepstudy` (via the helper) and rendered by WebView2; a coarse DRIPS figure
  is best-effort scanned for the summary.

## Verification status & hardware requirements

Verified on a dual-monitor, S3-only (no Modern Standby) Windows 11 box (two identical
Gigabyte G27Q panels, distinct connector UIDs):

- **Verified working here:** Layout Keeper startup capture (9 windows, two-monitor
  topology, EDID + device-path identity, HWND persisted for same-session matching), Sleep
  Doctor collection + event-log timeline + report generation, adaptive debounce (800ms for
  2 monitors), JSON store round-trip, named-pipe IPC framing, single-instance activation,
  `GUID_MONITOR_POWER_ON` handling, and the full test harness (100 checks).
- **Window coverage, verified on a three-monitor box (v0.4):** the keeper's capture now
  matches the full taskbar-visible window set on a live desktop (28/28, including File
  Explorer windows that older builds dropped). A Win+Arrow snapped window restores to its
  exact snapped rect, including when it was re-snapped elsewhere first; a borderless
  fullscreen RDP (`mstsc`) window was moved to another monitor and restored correctly;
  seven identically titled Sticky Notes pair with their own saved positions.
- **Not verifiable on a single-desktop box:** keeping windows that sit on another virtual
  desktop. The code admits shell-cloaked windows that `IVirtualDesktopManager` places on
  a non-current desktop; it has not been exercised with a second desktop.
- **Collapse/restore on this box:** the two panels are always present in this session, so a
  real sleep-collapse was not reproduced here. Collapse detection, global match assignment,
  coordinate scaling, and same-model collision tie-break are covered by unit tests against
  synthetic topologies. A real dock/undock or monitor-sleep collapse still wants a physical
  unplug (or an IDD virtual display) to watch windows flee and come back.
- **Needs Modern Standby hardware to exercise:** SleepStudy/DRIPS. This box is S3-only, so
  the tool correctly reports "legacy S3 - not applicable." DRIPS data only appears on a
  Modern Standby (S0 low power idle) laptop/tablet.
- **Not auto-verifiable in a headless agent shell:** live GUI rendering (tray menus, label
  editor, the WebView2 window, one-click Disable wake) and the elevated UAC round-trip.
  These work when the app is run interactively on a normal desktop; the automated harness
  does not click them.

## Not yet implemented (v2+)

systemsleepdiagnostics detail, restoring a window *to* its original virtual desktop
(windows on other desktops are captured and repositioned, but desktop membership is not
saved), exclude-rule UI, IDD
virtual-display collapse prevention, ARM64. See the design doc roadmap.
