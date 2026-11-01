# How ClaudeStick works

## Pieces

```
Claude Code (CLI) --hooks--> claudestick_hook.py --HTTP :7880--> Stick firmware (ESP32-S3)
       |                                                            |  screen, buttons, buzzer
       +--status line--> claudestick_hook.py --statusline ---------+  Bluetooth <-> Claude desktop
                                                                    |  phone page, setup wizard
Claude desktop app <------ Bluetooth (Hardware Buddy protocol) ----+
```

- **Firmware** (`firmware/`): Arduino/PlatformIO for the StickS3. `src/main.cpp` holds the state, HTTP API and UI; `voice.inc` push-to-talk; `update.inc` update checks, install and the boot guard; `bridge.inc` the client for a bridge; `setup_page.html` the setup wizard; `embed_files.py` embeds the installer, hook and wizard into the image at build time so the Stick can serve them.
- **Bridge** (`bridge/claudestick_bridge.py`): the Stick's API without the hardware, for use away from home. Standard-library Python; see [REMOTE.md](REMOTE.md).
- **Telemetry collector** (`telemetry/claudestick_telemetry.py`): receives opt-in usage reports. Standard-library Python; see [TELEMETRY.md](TELEMETRY.md).
- **Hook** (`hooks/claudestick_hook.py`): one standard-library Python script registered for the Claude Code hook events and as the status-line command. It never blocks Claude when the Stick is unreachable (it backs off for 20 s after a failure).
- **Installer** (`install.py`): served by the Stick, so a new machine needs only a `curl | python3`. It edits `~/.claude/settings.json` (with a backup), writes `~/.claudestick/config.json`, and restores everything on `--uninstall`.
- **Web flasher** (`web/`): ESP Web Tools (vendored) plus `manifest.json` and `ota.json`, published to GitHub Pages by CI.

## Hook events and what the hook returns

| Event | What the hook does |
|---|---|
| `PermissionRequest` | Posts the request to `/permission`, polls for the answer. Returns `decision.behavior` allow/deny (with `updatedPermissions` for *always allow*, or a message carrying a spoken reason). No answer in time falls through to the terminal prompt. |
| `Stop` | Posts `done`; with remote mode on, shows the Done menu and returns `{"decision":"block","reason":...}` so a macro or phone text makes Claude continue. |
| `PreToolUse` | Reports the tool (also checks the "stop now" flag, returning `continue:false`). In remote mode an `AskUserQuestion` is shown on the Stick and answered with `deny` plus a reason carrying the answers, so Claude receives them without the terminal dialog. |
| `PostToolUse` | Reports progress, tracks the to-do list (`TodoWrite` / `TaskCreate` / `TaskUpdate`), reads token usage from the transcript, delivers a queued mid-run macro through `additionalContext`. |
| `UserPromptSubmit`, `SessionStart` | Report state; inject the remote-mode instruction as `additionalContext` once per change. A typed prompt tells the Stick you're at the keyboard. |
| `Notification`, `SessionEnd` | Mark a session waiting or gone. |
| status line | `claudestick_hook.py --statusline` forwards context %, cost, model and the 5-hour/7-day limits to `/usage`, then runs the status line you had before and prints its output unchanged. |

All text sent for display is folded to ASCII (the Stick's font has no other glyphs); text sent back to Claude is never altered.

## HTTP API (port 7880)

Every call except the pages below carries `X-Token: <token>` (or `?t=`). Hooks poll; nothing holds a connection open.

| Endpoint | Purpose |
|---|---|
| `GET /health` | Version, device id, hook hash, heap and uptime. No auth needed for discovery fields (the bridge adds `"pair": true`). |
| `POST /pair/new`, `POST /pair` | Bridge only. `/pair/new` (token needed) makes a one-time 6-digit code, `{"code", "expires_in"}`. `/pair` (no token) trades `{"code"}` for `{"ok", "token", "id"}`; wrong, expired and missing codes all answer `403` alike, a locked-out address `429`. |
| `POST /event`, `POST /usage` | Session state; context and plan limits. |
| `POST/GET/DELETE /permission`, `/stop` | Create, poll and cancel approvals and Done/Question cards. |
| `GET /state`, `POST /act` | What the phone page shows; its actions (approve, deny, choose, say, macro, stop, unsend, pin, remote). A session with a message waiting for Claude's next step carries `queued` (clipped to 60 characters); `unsend` takes it back. The bridge also sends `idle_s` per session (seconds since its hook last reported), which is how the Stick decides which session "the active one" is. |
| `GET/POST /config` | Settings shared with every hook (macros, waits, voice, ntfy). |
| `POST /stt` | Bridge only: WAV in, `{"text":...}` out. |
| `POST /ota` | Upload a firmware image by hand. |
| `GET /`, `/setup`, `/install.py`, `/claudestick_hook.py` | Phone page, setup wizard, installer, hook. |
| `/setup/info`, `/setup/scan`, `/setup/wifi`, `/setup/wifi/add`, `/setup/wifi/forget`, `/setup/bridge`, `/setup/preview`, `/setup/time`, `/setup/save` | The setup wizard's backend; open until setup is finished, then token-gated. |

## The bridge

`Core` in `claudestick_bridge.py` ports the firmware's session, approval and Done-card logic (same ids-by-poll protocol, same 15 s staleness, same pinning), and the HTTP layer adds what a network service needs: token check in constant time, per-address lockout, size limits, a connection cap, private files for the token and settings, optional TLS. `tests/py/contract_bridge.py` replays one scripted conversation against the bridge and the firmware mock and requires identical answers.

Differences from the Stick, all additive:

- **State versions.** `/state` carries `v`, which changes whenever anything that matters changes (computed by comparing snapshots, so no code path can forget to bump it). `GET /state?v=N` answers `{"v":N,"same":true}` when nothing changed: that is the Stick's normal poll, a few dozen bytes over a kept-alive connection. `?n=1` is the Stick's view: its newest 6 sessions, oldest 8 approvals, 4 cards, clipped text.
- **Presence.** Whoever polls `/state` (the Stick, the phone page) counts as connected for 30 s. Effective *remote* is the remote setting and the auto rule (answered on the Stick or phone more recently than typed at the keyboard) *and* someone connected, so a switched-off Stick never leaves prompts waiting.
- **Ids** start at 1,000,000 so they can't collide with a directly connected Stick's.
- **`/stt`** takes a WAV and forwards it, as multipart with the model and language from the bridge's settings and the key from the bridge's settings, to an OpenAI-compatible endpoint.
- **`/config`** hides secrets unless `?secrets=1` (the hook asks; the page and the Stick don't). `public_url` is what ntfy buttons and the phone link use.

### The Stick as a client (`bridge.inc`)

`bridgeTick()` runs on the main loop: GET `/state?n=1&v=<held>` every 1.5 s (4 s dimmed), over one kept-alive `HTTPClient` with the Mozilla root bundle. `bridgeApply()` mirrors sessions, approvals, cards, macro labels, limits and tokens into the same tables hooks fill, flagged `bridge`, so every screen works unchanged. Entries are replaced wholesale on each full state; hook-side expiry skips them. Alerts are derived from changes (new approval, session done, to-do finished) and suppressed for the first state after connecting. Answers go through small functions (`decide`, `stopResult`, `sessionStop`, `deliverText`) that act locally for hook entries and POST `/act` for bridge entries; a failed POST leaves the card up. Two failed polls drop everything from the bridge and start a back-off (3 s doubling to 60 s). TLS refuses to start below 32 KB of free internal RAM and waits for the clock.

Wi-Fi: up to four networks are kept (`nets` in preferences; the first is also in `ssid`/`pass` for older tools). With more than one, a disconnected Stick runs an asynchronous scan and joins the strongest known network, taking turns when none is in sight.

## Telemetry (opt-in, off by default)

`telemetry.inc` holds a window of counts (`TelCounters tel`, zeroed on a successful report) incremented at the call sites that matter — `decide()` for approvals, `stopResult()` for Done/Question cards, `findSession()` for new sessions, `voiceFail()`/the transcription success path for voice, and the two macro-tap sites — plus a POST every ~6 h (same idle-gated scheduling as update checks) once both `conf.telemetryOn` and `conf.telemetryUrl` are set. `telemetryId()` is a separate random id from `devId` (which is MAC-derived and used for mDNS), so a report carries nothing hardware-identifying. The collector (`telemetry/claudestick_telemetry.py`) enforces its own copy of the schema server-side — an unknown or mistyped field is dropped before it's stored, so a report can never carry more than `docs/TELEMETRY.md` documents even from a modified Stick. See that doc for the full field list and exactly what's never sent.

## Updates and rollback

- `ota.json` (`version`, `app`, `sha256`, `notes`) and `firmware/claudestick-app.bin` are published next to the flasher. The Stick compares versions, shows the changelog bullets for that version from `CHANGELOG.md`, and installs with the ESP `Update` library over TLS verified against `firmware/data/cert/x509_crt_bundle.bin` (regenerate with `tools/gen_ca_bundle.sh`).
- The flash layout is `default_8MB.csv`: two OTA slots plus `otadata`. An update only writes the inactive slot and switches over when the image verifies, so an interrupted download leaves the running firmware alone.
- Boot guard: each boot increments a counter that is cleared after 20 s of uptime. At 3, the Stick switches back to the other slot, records the failed version as skipped and shows a notice. With no valid other slot (right after a USB flash) it starts in safe mode instead (Bluetooth off).
- CI runs `tools/verify_release.py` before publishing: versions agree, the SHA-256 matches, the app fits the slot, the partition table has two OTA slots, and the update URL is baked into the image.

## Time

The clock is set from NTP when Wi-Fi is up, with the hook, the desktop app or the setup page as fallbacks. The time zone is stored as a POSIX rule (so daylight saving works); HTTPS needs the clock before it will trust a certificate, so update checks wait for it.

## Power

Screen brightness drops after 1 min and the screen turns off after 3 min; while it is dim or off, `motionWakeTick()` reads the accelerometer four times a second and `motionWakeStep()` wakes the screen on steady movement that follows a spell of lying still (single knocks and constant movement never do; the `MW_*` constants are untuned guesses, the `wake` setting turns it off); the CPU runs at 160 MHz (80 MHz while dimmed). Wi-Fi stays on so hooks and the bridge can reach the Stick; Bluetooth (for the desktop app) can be switched off in settings, which saves power and frees RAM for TLS. A drain meter (`drainTick`) tracks the battery level since it was unplugged and feeds the info screen's `batt` row and the serial `bat` command; `sleep max` is an experimental switch to `WIFI_PS_MAX_MODEM`, off by default because it hasn't been tried on hardware. If the heap fragments badly after days of uptime, the Stick restarts itself while idle.

## Repo map

```
firmware/        Arduino/PlatformIO project (src/, setup_page.html, embed_files.py, data/cert)
bridge/          claudestick_bridge.py (+ systemd unit example)
telemetry/       claudestick_telemetry.py (+ systemd unit example)
hooks/           claudestick_hook.py
install.py       installer served by the Stick
web/             web flasher (index.html, manifest.json, vendor/)
tools/           build_web.sh, verify_release.py, mock_device.py, gen_font.py, gen_tinyfont.py, gen_ca_bundle.sh
tests/           py/ (hook, installer, release checks, bridge, telemetry), host/ (firmware logic + screens on the host), browser/
docs/            screenshots and this document
.github/         CI: build, release checks, tests, Pages deploy
```
