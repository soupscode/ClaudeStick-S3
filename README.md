# ClaudeStick

**A pocket-size handheld for Claude Code.** Approve or deny tool calls with a button, see what every Claude session is doing, send one-button macros, talk to Claude, and keep an eye on your plan limits, without going back to the terminal. It runs on the **M5Stack StickS3** and works with the `claude` CLI on any machine on your network (over Wi-Fi) and with the Claude desktop app (over Bluetooth).

![ClaudeStick screens](docs/screens.png)

> **Status: early release.** The firmware is feature-complete and is developed against an automated suite (hook, installer and firmware-logic tests, plus a real compile in CI), but on-device behavior is still being validated on StickS3 hardware. Please [open an issue](../../issues) if something misbehaves.

## What it does

- **Approvals:** every permission prompt appears on the Stick. Allow, deny (optionally by voice, with a reason), or "always allow" with a rule that fits the request. Dangerous commands get a red card and a deliberate double-press.
- **Live status:** one row per Claude session (`folder@branch`) with a spinner, elapsed time, Claude's to-do progress, and a corner buddy that reacts to what's happening.
- **Usage at a glance:** context fill, 5-hour and 7-day plan limits (Pro/Max), session cost and model.
- **Macros and remote mode:** "Continue", "Next phase", "Run tests + fix", your own. In remote mode Claude is told you're on a tiny device and asks short multiple-choice questions you answer with a button.
- **Push-to-talk:** hold a button, speak, confirm; the text goes to Claude (needs any OpenAI-compatible speech-to-text server).
- **Phone page and push alerts:** a web page served by the Stick mirrors everything, and [ntfy](https://ntfy.sh) notifications carry Approve/Always/Deny buttons.
- **Away from home:** a small bridge at home relays your sessions, so the Stick (or your phone) works from the office or a phone hotspot. See [docs/REMOTE.md](docs/REMOTE.md).
- **Updates over Wi-Fi:** the Stick offers new versions with a changelog, installs them in one press, and rolls back by itself if a build won't boot.

## What you need

- An **M5Stack StickS3** (the K150, with the 1.14" screen) and a USB-C **data** cable.
- **Desktop Chrome or Edge** for the web flasher.
- **Python 3** on each machine where you run `claude` (the hook is a single standard-library script).
- Optional: a speech-to-text server for voice, an ntfy topic for phone alerts.

## Quick start

1. **Flash.** Open the web flasher: <https://soupscode.github.io/ClaudeStick-S3/> (a fork publishes its own at `https://<owner>.github.io/<repo>/`). Click **Install** (accept *erase* on the first install), then **Connect to Wi-Fi**, then **Visit device**. No computer handy? A fresh Stick opens a hotspot named `ClaudeStick-XXXX`; join it from your phone and the setup page appears.
2. **Set up.** The page at `http://<stick-ip>:7880/setup` asks for your time zone (detected for you), a name, a buddy, volume and a few options. The Stick shows nothing to configure on its own screen.
3. **Connect your machines.** The last step of setup prints one command with your IP and token filled in; run it on every machine where you use `claude`:

   ```bash
   curl -s http://<stick-ip>:7880/install.py | python3 - --host <stick-ip> --port 7880 --token <token>
   ```

   It backs up `~/.claude/settings.json`, adds its hooks next to yours, keeps any status line you already have, and is safe to re-run. `--test` re-checks the connection; `--uninstall` removes everything it added.
4. **Claude desktop app (optional).** Help → Troubleshooting → Enable Developer Mode → Developer → Open Hardware Buddy… → Connect, pick your Stick, enter the 6-digit code it shows. The desktop gives status and approve/deny; macros and the rest need the CLI hooks.
5. **Check it works.** On the Stick: **menu → hardware test** shows the buttons, tilt, battery, Wi-Fi and Bluetooth live. Then trigger a Claude Code approval.

## Using it

**A** is the front button, **B** the side button, **P** the power button (short press); *hold* is about 0.7 s and *2x* a double-press within 0.35 s. Hold A is always **talk**.

| Screen | A | 2x A | hold A | B | hold B | P |
|---|---|---|---|---|---|---|
| Approval | allow | **always allow** | **deny + say why** | deny | **answer at keyboard** | answer at keyboard |
| Approval, **danger** (red) | hint only | allow once | deny + say why | deny | answer at keyboard | answer at keyboard |
| Question / Done menu | pick | | **talk** | next | dismiss | previous |
| Home | **session details** | | **talk to Claude** | menu | menu | |
| Session details | back | | talk to this session | next session | back | previous session |
| Menu / lists | select | | | next | back | previous |
| Voice result | send | | redo | discard | discard | |

On cards that offer *always allow*, a single A waits 0.35 s to see whether a second press follows. A hint bar along the bottom always shows what the buttons do right now. **Menu → button help** lists them on the device.

### Approvals

- **Always allow** remembers a rule that fits: `Bash(git status *)` for this project, auto-accept edits for the session, a `WebFetch` domain, or an MCP tool. The card's last line shows exactly what will be remembered. It is never offered for compound shell commands or dangerous ones.
- **Danger cards** (red border) cover `rm -rf`, force pushes, `git reset --hard`, `zfs`/`zpool destroy`, `pct`/`qm destroy`, `dd` to a disk, `mkfs`, `curl | sh`, `DROP TABLE`, stopping SSH or network services, and edits to `/etc`, `~/.ssh` or `.env`. These are heuristics, not a security boundary. Review `.claude/settings.local.json` now and then.
- **Deny + say why:** hold A, say what Claude should do instead, press A. Claude gets the denial with your words. The phone has a matching **Deny + note** button.
- **Edits** show a red/green diff; long text scrolls when you tilt the Stick.

### Remote mode

Set it from the menu, the phone page or settings: **auto** (default), **on**, or **off**. Auto follows where you last answered: an answer from the Stick or phone turns it on, typing in the terminal turns it off.

| | Remote **on** (away from the desk) | Remote **off** (at the desk) |
|---|---|---|
| Claude is told | You're on a 2-button device: ask with multiple-choice questions, keep summaries short, end each turn with `Options: a \| b \| c`, batch risky steps | Back to normal |
| Claude's questions | A card on the Stick or phone; your pick is the answer | The normal terminal dialog |
| When Claude finishes | A Done menu: Claude's suggestions, then your macros (waits up to 10 min) | Nothing; never blocks your keyboard |
| Approvals | Wait up to 10 min on the Stick/phone (plus an ntfy push if configured) | Wait 20 s, then the normal terminal prompt |

Claude following the remote-mode style is an instruction, not a guarantee; if it skips the `Options:` line the Done menu just shows your macros.

### Sessions, to-do list and usage

Every terminal with the hooks shows up on Home as `folder@branch` (set `CLAUDESTICK_NAME` to label one yourself). Tap A to open the focused session: Claude's to-do list with a progress bar, tokens used, context fill, 5-hour and 7-day limits with reset countdowns, cost and model. **Menu → tie to session** pins one session so the Stick only answers that one and every other session falls through to its own terminal. When Claude ticks off its last to-do item the Stick chimes (and sends an ntfy push in remote mode).

- **To-do list:** Claude only keeps one when its to-do tools are loaded, and newer models don't load them by default, so the installer sets `CLAUDE_CODE_ENABLE_TODO_TOOLS=1` unless you've decided either way (`--no-todo-tools` skips it).
- **Plan limits:** these exist only in Claude Code's status-line data, so the installer sets a small status-line command. An existing status line is wrapped and keeps printing exactly what it did; uninstall restores it. Without one it prints `ctx 42% | 5h 63% | 7d 28%`. `--no-statusline` skips this and the Stick shows tokens only. Limits are Pro/Max only and appear after the first response; they dim after 15 minutes without an update.

### Push-to-talk

Hold A, speak (up to 20 s), release, review the text, press A to send. It answers a pending Done/Question card, otherwise Claude gets it after its current step. Any OpenAI-compatible `/v1/audio/transcriptions` endpoint works; [speaches](https://github.com/speaches-ai/speaches) (faster-whisper) on any machine on your network is a good choice, and `Systran/faster-whisper-small` is fine on a CPU. Set the URL under **settings → voice**.

### Phone page

Open `http://<stick-ip>:7880/?t=<token>` once and add it to your home screen. It approves, denies, answers questions, taps macros, stops a session and sends free text. For use away from home, run the [bridge](docs/REMOTE.md): it serves the same page at an HTTPS address of your choosing, with or without a Stick. A VPN such as Tailscale works too, or use Claude Code's built-in Remote Control to type into a session from the Claude app.

### Settings

Open `http://<stick-ip>:7880/setup?t=<token>` (the phone page links to it). Every machine's hook fetches these from the Stick, so you edit them once: macros (`label | instruction`, up to 10), approval waits, the Done menu mode, voice, ntfy, time zone and clock style, text style, volume, tilt, and firmware updates. The menu on the Stick also has the common toggles (remote mode, buddy, text style, 12/24-hour clock, flip screen).

### Away from home

Run `python3 bridge/claudestick_bridge.py` on a computer at home, point your machines' hooks at it, expose it with an HTTPS reverse proxy (or a VPN), and give the Stick the bridge's address, token and your phone's hotspot as a second Wi-Fi network. Approvals, Done cards, questions, voice, macros and usage then work from any network. Everything is in [docs/REMOTE.md](docs/REMOTE.md).

## Updates

The Stick checks your update source (the flasher site) 90 s after boot and every 6 hours, waiting for a quiet moment. A newer version shows **update available!** with the changelog and three choices: **update now**, **remind me later** (24 h), **skip this version**. *Menu → check for updates* checks on demand. Downloads are HTTPS verified against an embedded Mozilla root bundle. A boot only counts as good after 20 s of running; three bad boots in a row switch back to the previous firmware and skip the failed version. Each machine's hook also updates itself to match the Stick. If the Stick's IP changes, the hook finds it again by its device id.

## Troubleshooting and recovery

- **No serial port in the flasher:** try another cable; it must carry data.
- **Flashing fails:** hold the side button until the green LED blinks (download mode), then retry.
- **Upside down:** menu → *flip screen*. If tilt scrolls the wrong way, run `tilt flip` in the flasher's *Logs & Console*.
- **Title says `claudestick:safe`:** Bluetooth was switched off because the Stick crashed repeatedly on boot. Open *Logs & Console* and run `info`; the "last reset" line says why. Include it in a bug report.
- **A machine can't reach the Stick:** re-run the install command with `--test`; it says whether the Stick answers and whether the token is accepted. Check that both are on the same network. If the IP changed, the hook finds the Stick again by its device id at the next prompt.
- **Several Wi-Fi networks:** the Stick remembers up to four (settings → wi-fi networks) and joins the strongest one in range.
- **Serial commands** (*Logs & Console* or any serial monitor): `info`, `wifi <ssid> <pass>`, `token <t>`, `name <n>`, `tilt on|off|flip`, `flip`, `tz <POSIX rule>`, `clock 12|24`, `font 0|1`, `setup`, `forget`, `reboot`.
- **Hold B at power-on** starts in safe mode (no Bluetooth); **hold A+B at power-on** forgets Wi-Fi and reruns setup.
- **You can't brick it with firmware.** The ESP32-S3's first-stage bootloader is in ROM, so download mode (hold the side button while plugging in USB) always works. From there, flash again from the web flasher, `pio run -t upload`, or erase with `esptool.py --chip esp32s3 erase_flash`. This firmware never touches eFuses (secure boot or flash encryption), the only way to lock an ESP32 permanently.

## Privacy and security

Nothing leaves your network except what you configure: update checks go to your update source, voice audio to your speech-to-text server, ntfy messages to your ntfy server. The Stick and the hooks talk over **plain HTTP with a shared token**, so keep the Stick on a trusted network. For use away from home the bridge adds HTTPS (through your reverse proxy), a per-address lockout and size limits; anyone with its token can approve what Claude runs, so treat it like an SSH key. Until setup is finished, `/setup` is open to anyone on the network or hotspot so it can show you the token; finish setup promptly. Firmware updates are not code-signed: they are trusted because they come from your update source over verified HTTPS, so protect the repo and site that serve them. See [SECURITY.md](SECURITY.md).

## How it works, and developing it

The hooks POST events to the Stick (or to the bridge, which the Stick then polls), which polls rather than holds connections open; plan limits arrive through the status line. The design, API and hook outputs are in [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md). To build, test, release or contribute, see [CONTRIBUTING.md](CONTRIBUTING.md). You can try everything without hardware: `python3 tools/mock_device.py --token test123` serves the Stick's API, phone page and setup wizard on your machine.

Fork it freely: CI bakes `https://<owner>.github.io/<repo>/` into the firmware as its update source, so a fork's Sticks update from the fork. Enable Pages (Source: GitHub Actions), push, and the flasher and updates work. Change the copyright holder in `LICENSE` and the wording in `web/index.html`.

## Credits and license

MIT, see [LICENSE](LICENSE). `firmware/src/ble_bridge.*` and `firmware/src/buddies/*` come unmodified from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (MIT); `buddy.cpp` is adapted from it. `firmware/src/tinyfont.h` is derived from the 3x5 "Tom Thumb" font (BSD 3-clause, license in the file). The web flasher uses [ESP Web Tools](https://github.com/esphome/esp-web-tools), vendored in `web/vendor/`. Built on [M5Unified](https://github.com/m5stack/M5Unified), [M5GFX](https://github.com/m5stack/M5GFX) and [ArduinoJson](https://arduinojson.org).
