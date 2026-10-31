# How ClaudeStick works

## Pieces

```
Claude Code (CLI) --hooks--> claudestick_hook.py --HTTP :7880--> Stick firmware (ESP32-S3)
       |                                                            |  screen, buttons, buzzer
       +--status line--> claudestick_hook.py --statusline ---------+  Bluetooth <-> Claude desktop
                                                                    |  phone page, setup wizard
Claude desktop app <------ Bluetooth (Hardware Buddy protocol) ----+
```

- **Firmware** (`firmware/`): Arduino/PlatformIO for the StickS3. `src/main.cpp` holds the state, HTTP API and UI; `voice.inc` push-to-talk; `update.inc` update checks, install and the boot guard; `setup_page.html` the setup wizard; `embed_files.py` embeds the installer, hook and wizard into the image at build time so the Stick can serve them.
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
| `GET /health` | Version, device id, hook hash, heap and uptime. No auth needed for discovery fields. |
| `POST /event`, `POST /usage` | Session state; context and plan limits. |
| `POST/GET/DELETE /permission`, `/stop` | Create, poll and cancel approvals and Done/Question cards. |
| `GET /state`, `POST /act` | What the phone page shows; its actions (approve, deny, choose, say, macro, stop, pin, remote). |
| `GET/POST /config` | Settings shared with every hook (macros, waits, voice, ntfy). |
| `POST /ota` | Upload a firmware image by hand. |
| `GET /`, `/setup`, `/install.py`, `/claudestick_hook.py` | Phone page, setup wizard, installer, hook. |
| `/setup/info`, `/setup/scan`, `/setup/wifi`, `/setup/preview`, `/setup/time`, `/setup/save` | The setup wizard's backend; open until setup is finished, then token-gated. |

## Updates and rollback

- `ota.json` (`version`, `app`, `sha256`, `notes`) and `firmware/claudestick-app.bin` are published next to the flasher. The Stick compares versions, shows the changelog bullets for that version from `CHANGELOG.md`, and installs with the ESP `Update` library over TLS verified against `firmware/data/cert/x509_crt_bundle.bin` (regenerate with `tools/gen_ca_bundle.sh`).
- The flash layout is `default_8MB.csv`: two OTA slots plus `otadata`. An update only writes the inactive slot and switches over when the image verifies, so an interrupted download leaves the running firmware alone.
- Boot guard: each boot increments a counter that is cleared after 20 s of uptime. At 3, the Stick switches back to the other slot, records the failed version as skipped and shows a notice. With no valid other slot (right after a USB flash) it starts in safe mode instead (Bluetooth off).
- CI runs `tools/verify_release.py` before publishing: versions agree, the SHA-256 matches, the app fits the slot, the partition table has two OTA slots, and the update URL is baked into the image.

## Time

The clock is set from NTP when Wi-Fi is up, with the hook, the desktop app or the setup page as fallbacks. The time zone is stored as a POSIX rule (so daylight saving works); HTTPS needs the clock before it will trust a certificate, so update checks wait for it.

## Power

Screen brightness drops after 1 min and the screen turns off after 3 min; the CPU runs at 160 MHz (80 MHz while dimmed). Wi-Fi and Bluetooth stay on so hooks and the desktop can still reach the Stick. If the heap fragments badly after days of uptime, the Stick restarts itself while idle.

## Repo map

```
firmware/        Arduino/PlatformIO project (src/, setup_page.html, embed_files.py, data/cert)
hooks/           claudestick_hook.py
install.py       installer served by the Stick
web/             web flasher (index.html, manifest.json, vendor/)
tools/           build_web.sh, verify_release.py, mock_device.py, gen_font.py, gen_tinyfont.py, gen_ca_bundle.sh
tests/           py/ (hook, installer, release checks), host/ (firmware logic + screens on the host), browser/
docs/            screenshots and this document
.github/         CI: build, release checks, tests, Pages deploy
```
