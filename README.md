# OhMyDisplay

Windows tray utility that solves two chronic display/power annoyances with one tool:

1. **Layout Keeper** - auto-saves window layouts per display topology and restores them
   after a monitor sleep/input-switch collapses everything onto one screen.
2. **Sleep Doctor** - collects fragmented `powercfg` / event-log power signals into one
   plain-language report with one-click safe fixes.

Implements the v0 (MVP) scope of [`OhMyDisplay-design.md`](OhMyDisplay-design.md).

## Status

v0 MVP + initial v1 features, building and running. Single portable exe, no .NET / external
runtime. Zero `/W4 /permissive-` warnings; 46 test checks pass.

| Component | State |
|---|---|
| Core (event bus, CCD topology + identity, power notify, JSON store, atomic write, logger) | done |
| Layout Keeper (identity, adaptive sliding debounce, capture, restore + verify/retry, labels, ghost detect) | done |
| Sleep Doctor (powercfg collectors + locale-robust parsers, event-log correlator, rule engine, HTML/text report) | done |
| Sleep architecture + SleepStudy/DRIPS (Modern Standby detection, official report via helper, graceful S3 degrade) | done (v1) |
| Embedded report window (WebView2 when available, native text fallback) | done (v1) |
| Tray shell (icon, menu, balloons, global hotkeys, monitor label editor) | done |
| Elevated helper (`requireAdministrator`, named-pipe IPC, safe powercfg actions, admin collection) | done |
| Test harness (debounce, JSON, signature, collision tie-break, coord round-trip, match scoring, IPC framing, live doctor) | done |

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

## Usage

Run `OhMyDisplay.exe`. It lives in the tray. Right-click the icon for:

- **Save / Restore layout now** (also `Ctrl+Alt+S` / `Ctrl+Alt+R`)
- **Sleep Doctor report** - opens an HTML report in the default browser
- **Sleep Doctor report (full, admin)** - additionally collects admin-only sections
  (`/requests`, `/waketimers`) through the elevated helper over named-pipe IPC; prompts
  for consent once via UAC
- **Monitor labels** - name each monitor; required to disambiguate identical models
- **Detect ghost monitors** - lists phantom display entries the system still remembers

Layouts are saved automatically when a new topology is first seen, and restored after an
adaptive debounce when a known topology reappears (e.g. after monitor sleep/resume).

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

Verified on a single-monitor, S3-only (no Modern Standby) Windows 11 dev box:

- **Verified working here:** Layout Keeper capture/save/restore (11/11 windows restored on a
  real topology, EDID-based identity), Sleep Doctor collection + event-log timeline + report
  generation, adaptive debounce, JSON store round-trip, named-pipe IPC framing, and the full
  test harness (46 checks).
- **Needs 2+ monitors to exercise:** the headline collapse/restore behavior. With one monitor
  there is no other screen for windows to flee to, so a real collapse cannot occur. The
  multi-monitor logic (topology signature order-independence, same-model collision tie-break,
  cross-monitor coordinate scaling, restore matching) is covered by unit tests against
  synthetic topologies, but real docking/undocking behavior requires a multi-monitor setup
  (or a virtual display driver such as an IDD-based one).
- **Needs Modern Standby hardware to exercise:** SleepStudy/DRIPS. This dev box is S3-only, so
  the tool correctly reports "legacy S3 - not applicable." DRIPS data only appears on a
  Modern Standby (S0 low power idle) laptop/tablet.
- **Not auto-verifiable in a headless agent shell:** live GUI rendering (tray menus, label
  editor, the WebView2 window) and the elevated UAC round-trip. These work when the app is
  run interactively on a normal desktop; only the automated harness could not click/observe
  them.

## Not yet implemented (v2+)

systemsleepdiagnostics detail, virtual-desktop membership, exclude-rule UI, IDD
virtual-display collapse prevention, ARM64. See the design doc roadmap.
