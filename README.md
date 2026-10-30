# ClaudeStick

Approvals, live status and one-button macros for Claude on an **M5Stack StickS3** (K150).

| Source | Link | What works |
|---|---|---|
| Claude desktop app (Cowork, Claude Code in the app), Mac/Windows | Bluetooth (Anthropic's Hardware Buddy API) | Status, approve/deny |
| `claude` CLI on any machine/LXC on your LAN | Wi-Fi (Claude Code hooks) | Status, approve/deny, **macros**, **remote stop** |

Both work at once. Approvals from either source queue on the same screen. A **phone page** served by the Stick (`http://<stick-ip>:7880/?t=<token>`) mirrors everything: approve/deny, answer questions, tap macros, stop, and type free-text instructions.

## Remote mode

Set it in the menu (hold B → *remote*), on the phone page, or in settings: **auto** (default), **on**, or **off**. In auto it follows where you last answered: an answer from the Stick or phone turns it on, and typing a prompt in the terminal turns it off.

| | Remote mode **ON** (away from desk) | Remote mode **OFF** (at desk, default) |
|---|---|---|
| Claude is told | You're on a 2-button device: ask decisions with multiple-choice questions, keep summaries short, end each turn with `Options: a \| b \| c`, batch risky steps | Back to normal |
| Claude's multiple-choice questions | Shown as a card on the Stick/phone; your pick goes back as the answer | Normal terminal UI |
| When Claude finishes | Done menu: Claude's own suggested options first, then your macros, plus free text from the phone (waits up to 10 min) | No menu, never blocks your keyboard |
| Approvals | Wait up to 10 min on Stick/phone (+ phone push if ntfy is set) | Wait 20 s, then the normal terminal prompt |

The mode switch reaches Claude at the next hook (tool result, prompt, or Done-menu pick), and only once per change.

## Screen and buttons

The UI is a little terminal pane: one of Anthropic's 18 animated ASCII buddies on the left (from claude-desktop-buddy), content on the right, and key hints on the bottom border. The buddy reacts: busy while Claude works, attention when it needs you, a celebration when it finishes, dizzy on errors, hearts when you approve. Pick a species from the menu. Previews are in `docs/screens.png` and `docs/buddies.png`.

**A** = front button, **B** = side button, **P** = power button (short press), *hold* ≈ 0.7 s.

| Screen | A | hold A | B | hold B | P |
|---|---|---|---|---|---|
| Approval | allow | **always allow** (writes a permission rule) | deny | — | answer at keyboard |
| Approval, **DANGER** (red) | (nudge) | allow once | deny | — | answer at keyboard |
| Question / Done menu | pick | **talk** (spoken reply) | next | dismiss | previous |
| Home | next session | **talk to Claude** | — | **menu** | — |
| Menu / lists | select | — | next | back | previous |
| Voice result | send | redo | discard | discard | — |

- **Always allow** remembers a rule that fits the request:
  - `Bash(git status *)` for this project (`.claude/settings.local.json`)
  - auto-accept edits for the session
  - a `WebFetch` domain
  - an MCP tool

  The card's last line shows exactly what will be remembered. It isn't offered for compound shell commands or dangerous ones.
- **DANGER cards** (red) need a hold to allow, and never offer "always". Triggers include:
  - `rm -rf`, force pushes, `git reset --hard`
  - `zfs`/`zpool destroy`, `pct`/`qm destroy`
  - `dd` to a disk, `mkfs`, `curl | sh`, `DROP TABLE`
  - stopping SSH, network or Proxmox services
  - edits to `/etc`, `~/.ssh` or `.env`

  Edit approvals show a red/green diff.
- **Tilt to scroll:** tip the Stick forward or back on long commands. Use the serial command `tilt flip` or `tilt off` if it feels wrong.
- **Menu:** stop now · your macros · remote (auto/on/off) · tie to session · buddy · button help · info · new token (hold A) · re-run setup.

### Push-to-talk

Hold A, speak (up to 20 s), and release. The Stick sends the audio to a speech-to-text server, shows you the text, and A sends it. It answers the Done/Question card if one is up; otherwise Claude gets it after its current step. Any OpenAI-compatible `/v1/audio/transcriptions` endpoint works. On the homelab, run [speaches](https://github.com/speaches-ai/speaches) (faster-whisper) in an LXC; `Systran/faster-whisper-small` is fine on CPU. Then set the URL under **settings → voice**.

### Choosing the session

Every terminal with the hooks installed shows up on Home, named `folder@branch` (the branch is left off for main/master). Set `CLAUDESTICK_NAME=whatever` before `claude` to label a session yourself. Two sessions in the same folder get a `#abcd` suffix.

- **A** on Home moves the focus (underlined). The menu's Stop and macros, and the phone's default target, all go to the focused session.
- **Menu → tie to session** pins one session (`*`). The Stick then only answers that session: approvals, questions and Done menus from other sessions fall straight through to their own terminals. Choose *all sessions* to unpin. The phone page has the same Pin/Unpin buttons.

## Setup

### 1. Get the web flasher online (once)

The flasher page is `web/`. It needs a compiled firmware image next to it. Pick one:

- **GitHub (recommended):** push this repo to GitHub, then in Settings → Pages set Source to **GitHub Actions**. `.github/workflows/firmware.yml` compiles the firmware on every push and publishes the flasher at `https://<you>.github.io/<repo>/`. The compiled `.bin` is also attached to each run as an artifact.
- **Local:** `pip install platformio "esptool<5"`, then run `./tools/build_web.sh` and `python3 -m http.server -d web 8000`. Add `CS_UPDATE_URL=<your flasher URL>` to bake the update source in; it can also be set in settings. Open `http://localhost:8000`; localhost counts as secure, so USB flashing works.

ESP Web Tools is vendored in `web/vendor/`, so the page has no CDN dependency.

### 2. Flash + Wi-Fi (Chrome or Edge on desktop)

Plug the Stick in with a data cable and open the flasher. Click **Install**, then **Connect to Wi-Fi** (sent over USB with the Improv protocol), then **Visit device**.

No computer? A fresh Stick opens a hotspot named `ClaudeStick-XXXX`. Join it from your phone and the setup page pops up.

### 3. First-time setup (the Stick's own page)

`http://<stick-ip>:7880/setup` covers:

- **Name:** shows in Claude desktop's Bluetooth list, always prefixed `Claude-`.
- **Buddy:** previews live on the Stick as you pick.
- **Settings:** volume, tilt, and whether to start in remote mode.

The last step prints, with your IP and token filled in:

```bash
curl -s http://<stick-ip>:7880/install.py | python3 - --host <stick-ip> --port 7880 --token <token>
```

Run that on each machine where you use `claude`. The Stick serves the installer and hook itself, so no checkout is needed. The step also gives you the phone-remote link and the desktop pairing steps. The installer backs up `~/.claude/settings.json`, adds its hooks next to any you already have, and is safe to re-run. Use `--test` to re-check and `--uninstall` to remove.

The Stick only shows a "go here to set me up" screen until this is done. Its only button there is *hold B: skip setup*, for serial/CLI users. Afterwards, `/setup` needs `?t=<token>`. **Menu → re-run setup** reopens it. Holding **A+B while powering on** forgets Wi-Fi and starts over.

### 4. Desktop app (optional)

Claude desktop → **Help → Troubleshooting → Enable Developer Mode** → **Developer → Open Hardware Buddy…** → Connect → pick your Stick, then enter the 6-digit code it shows.

### Manual / serial fallback

`pio run -t upload` from `firmware/`, then in `pio device monitor`: `wifi <ssid> <pass>`, `info`, `setup`, `tilt on|off|flip`, `forget`, `reboot`.

### Recovery: you can't brick it with firmware

The ESP32-S3's first-stage bootloader is in mask ROM, so download mode always works no matter what's flashed. To enter it, plug in USB, then hold the side button until the internal green LED blinks. From there:

- `pio run -t upload` again, or
- restore stock: M5Burner → StickS3 → factory firmware → Burn, or
- `pip install esptool && esptool.py --chip esp32s3 erase_flash`, then flash anything.

This firmware never touches eFuses (secure boot / flash encryption), which is the only way to permanently lock an ESP32.

## Settings (on the Stick)

Open `http://<stick-ip>:7880/setup?t=<token>` (the phone page links to it). Every machine's hook fetches these settings from the Stick (cached for 60 s), so you edit them once:

- **Macros:** `label | instruction` per line, max 10.
- **Approvals:** desk wait (20 s), remote wait (600 s), Done menu (remote / always / never), and whether Claude's multiple-choice questions go to the Stick.
- **Voice:** transcription URL, model, language, API key.
- **Phone alerts:** an [ntfy](https://ntfy.sh) topic and token, with a *send test* button. In remote mode, approvals arrive as notifications with **Approve / Always / Deny** buttons. Those buttons call the Stick directly, so your phone must reach it (home Wi-Fi or Tailscale).
- **Firmware:** *check for update* reads `ota.json` from your flasher site and installs over Wi-Fi with a progress bar. You can also upload a `claudestick-app.bin` by hand.

### Updates

- **The prompt:** the Stick checks `<update source>/ota.json` 90 s after boot and every 6 h. Downloads are HTTPS, verified against an embedded Mozilla root bundle (`tools/gen_ca_bundle.sh` regenerates it). A newer version shows **update available!** with that version's bullets from `CHANGELOG.md`, and three choices: **update now** · **remind me later** (24 h) · **skip this version**. *Menu → check for updates* checks on demand.
- **Rollback:** a boot only counts as good after 20 s of running. Three bad boots in a row switch back to the previous firmware, skip the failed version, and show a "rolled back" notice. No USB needed.
- **Hooks follow the firmware:** each machine's hook compares itself with the copy the Stick serves (sha256) and replaces itself if it's out of date. It checks the hash and that the file parses before swapping.
- **Releasing:** bump `FW_VERSION` in `firmware/src/main.cpp`, add a `## x.y.z` section to `CHANGELOG.md`, and push. CI builds and publishes, and every Stick offers the update.

`~/.claudestick/config.json` on each machine only holds `host`, `port`, `token` and the Stick's `device_id`. **If the Stick's IP changes**, the hook looks for it by device id (mDNS name `claudestick-xxxx.local`, then a quick sweep of the local /24) at the next prompt, then updates the file itself. Tool calls never wait on that search.

## Phone

- **On your LAN:** open `http://<stick-ip>:7880/?t=<token>` once and add it to your home screen; it remembers the token. Free text you type is delivered at the next Done menu (instantly if one is up) or after Claude's current step. It can't start a turn while Claude is completely idle; use Remote Control for that.
- **Away from home:** use Claude Code's built-in **Remote Control** (`/remote-control`, or enable it for all sessions in `/config`) to type into the session from the Claude app. Or put the Stick's IP on Tailscale with your phone.

## How it works

The hooks POST events to the Stick (`/event`, `/permission`, `/stop`). Approvals and cards are polled, so nothing holds a connection open. The phone page uses `/state` and `/act`. Every API call carries an `X-Token` header. Hook outputs used:

- `PermissionRequest` → `decision.behavior` allow/deny, plus `updatedPermissions` for *always allow*
- `Stop` → `decision: "block"` + `reason` (this is how macros and phone text make Claude continue)
- `PostToolUse` / `UserPromptSubmit` / `SessionStart` → `additionalContext` (mid-run macros, remote-mode instructions)
- `PreToolUse` → `continue: false` (remote stop); for `AskUserQuestion` in remote mode, `deny` + a reason carrying your answers, so Claude gets them without the terminal dialog

## Test without hardware

```bash
python3 tools/mock_device.py --token test123     # a / b / k / d / s / m N / r / t TEXT at the prompt
# phone page: http://localhost:7880/?t=test123   setup wizard: http://localhost:7880/setup
python3 install.py --host 127.0.0.1 --token test123
```

## Notes / limits

- Macros, Stop, questions and remote mode only apply to CLI (hook) sessions. The desktop Bluetooth protocol only allows approve/deny.
- Claude following the remote-mode style (questions, `Options:` line) is an instruction, not a guarantee. If it skips the line, the Done menu just shows your macros.
- Desktop folder-push (custom GIF pets) isn't supported. The desktop will report that the device declined.
- Security: the HTTP API is plain HTTP with a shared token, so keep the Stick on a trusted VLAN. Before setup is finished, `/setup` is open to anyone on the network (and on the hotspot) so it can show you the token. Finish setup promptly.
- `firmware/src/ble_bridge.*` and `firmware/src/buddies/*` are unmodified from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (MIT). `buddy.cpp` is adapted from it, and `M5StickCPlus.h` is a small shim so the species render on M5Unified.
- The PWR short press reads the StickS3 power chip through M5Unified. A long PWR hold is still the hardware power-off.
- **Battery:** the screen dims after 1 min and turns off after 3 min, and the CPU drops to 80 MHz while dimmed. Wi-Fi and Bluetooth stay on so hooks and the desktop can still reach the Stick. Expect several hours unplugged; it's meant to live on a cable or magnetic dock.
- Always-allow rules and DANGER patterns are heuristics. Review `.claude/settings.local.json` now and then.

MIT License.
