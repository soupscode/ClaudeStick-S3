# How to use ClaudeStick

Everything after [installing](../README.md#install): the buttons, approvals, remote mode, sessions and usage, push-to-talk, the phone page, settings, updates and battery. For use away from home see [REMOTE.md](REMOTE.md); if something misbehaves see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## The buttons

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

## Approvals

- **Always allow** remembers a rule that fits: `Bash(git status *)` for this project, auto-accept edits for the session, a `WebFetch` domain, or an MCP tool. The card's last line shows exactly what will be remembered. It is never offered for compound shell commands or dangerous ones.
- **Danger cards** (red border) cover `rm -rf`, force pushes, `git reset --hard`, `zfs`/`zpool destroy`, `pct`/`qm destroy`, `dd` to a disk, `mkfs`, `curl | sh`, `DROP TABLE`, stopping SSH or network services, and edits to `/etc`, `~/.ssh` or `.env`. These are heuristics, not a security boundary. Review `.claude/settings.local.json` now and then.
- **Deny + say why:** hold A, say what Claude should do instead, press A. Claude gets the denial with your words. The phone has a matching **Deny + note** button.
- **Edits** show a red/green diff; long text scrolls when you tilt the Stick.

## Remote mode

Set it from the menu, the phone page or settings: **auto** (default), **on**, or **off**. Auto follows where you last answered: an answer from the Stick or phone turns it on, typing in the terminal turns it off.

| | Remote **on** (away from the desk) | Remote **off** (at the desk) |
|---|---|---|
| Claude is told | You're on a 2-button device: ask with multiple-choice questions, keep summaries short, end each turn with `Options: a \| b \| c`, batch risky steps | Back to normal |
| Claude's questions | A card on the Stick or phone; your pick is the answer | The normal terminal dialog |
| When Claude finishes | A Done menu: Claude's suggestions, then your macros (waits up to 10 min) | Nothing; never blocks your keyboard |
| Approvals | Wait up to 10 min on the Stick/phone (plus an ntfy push if configured) | Wait 20 s, then the normal terminal prompt |

Claude following the remote-mode style is an instruction, not a guarantee; if it skips the `Options:` line the Done menu just shows your macros.

## Sessions, to-do list and usage

Every terminal with the hooks shows up on Home as `folder@branch` (set `CLAUDESTICK_NAME` to label one yourself). Tap A to open the focused session: Claude's to-do list with a progress bar, tokens used, context fill, 5-hour and 7-day limits with reset countdowns, cost and model. **Menu → tie to session** pins one session so the Stick only answers that one and every other session falls through to its own terminal. When Claude ticks off its last to-do item the Stick chimes (and sends an ntfy push in remote mode).

- **To-do list:** Claude only keeps one when its to-do tools are loaded, and newer models don't load them by default, so the installer sets `CLAUDE_CODE_ENABLE_TODO_TOOLS=1` unless you've decided either way (`--no-todo-tools` skips it).
- **Plan limits:** these exist only in Claude Code's status-line data, so the installer sets a small status-line command. An existing status line is wrapped and keeps printing exactly what it did; uninstall restores it. Without one it prints `ctx 42% | 5h 63% | 7d 28%`. `--no-statusline` skips this and the Stick shows tokens only. Limits are Pro/Max only and appear after the first response; they dim after 15 minutes without an update.

## Push-to-talk

Hold A, speak (up to 20 s), release, review the text, press A to send. It answers a pending Done/Question card, otherwise Claude gets it after its current step. Any OpenAI-compatible `/v1/audio/transcriptions` endpoint works; [speaches](https://github.com/speaches-ai/speaches) (faster-whisper) on any machine on your network is a good choice, and `Systran/faster-whisper-small` is fine on a CPU. Set the URL under **settings → voice**. Use whatever model name your server expects: servers built on faster-whisper usually want a Hugging Face id such as `Systran/faster-whisper-small`, while servers that imitate OpenAI take `whisper-1` (the default). With a [bridge](REMOTE.md) the speech server is set on the bridge's settings page instead, and the Stick never talks to it directly.

## Phone page

Open `http://<stick-ip>:7880/?t=<token>` once and add it to your home screen. It approves, denies, answers questions, taps macros, stops a session and sends free text. For use away from home, run the [bridge](REMOTE.md): it serves the same page at an HTTPS address of your choosing, with or without a Stick. A VPN such as Tailscale works too, or use Claude Code's built-in Remote Control to type into a session from the Claude app.

## Settings

Open `http://<stick-ip>:7880/setup?t=<token>` (the phone page links to it). Every machine's hook fetches these from the Stick, so you edit them once: macros (`label | instruction`, up to 10), approval waits, the Done menu mode, voice, ntfy, time zone and clock style, text style, volume, tilt, and firmware updates. The menu on the Stick also has the common toggles (remote mode, buddy, text style, 12/24-hour clock, flip screen). The Stick remembers up to four Wi-Fi networks (**settings → wi-fi networks**), for example home and a phone hotspot, and joins the strongest one in range.

## Away from home

Run `python3 bridge/claudestick_bridge.py` on a computer at home, point your machines' hooks at it, expose it with an HTTPS reverse proxy (or a VPN), and give the Stick the bridge's address, token and your phone's hotspot as a second Wi-Fi network. Approvals, Done cards, questions, voice, macros and usage then work from any network. Everything is in [docs/REMOTE.md](REMOTE.md).

## Updates and rollback

The Stick checks your update source (the flasher site) 90 s after boot and every 6 hours, waiting for a quiet moment. A newer version shows **update available!** with the changelog and three choices: **update now**, **remind me later** (24 h), **skip this version**. *Menu → check for updates* checks on demand. Downloads are HTTPS verified against an embedded Mozilla root bundle. A boot only counts as good after 20 s of running; three bad boots in a row switch back to the previous firmware and skip the failed version. Each machine's hook also updates itself to match the Stick. If the Stick's IP changes, the hook finds it again by its device id.

## Battery

The StickS3 has a 250 mAh battery, so treat it as a desk device with a grace period rather than a full-day wearable. **Not measured on hardware yet**; from the chip's power figures and what the firmware does, expect roughly **4-8 hours** on battery with the screen mostly off (the low end if you use it a lot, or with a [bridge](REMOTE.md), which keeps the radio busier), and a full charge from USB-C in about an hour or two. Plugged in, it is fine to leave running all day. What helps: keep the volume modest (M5Stack advises under 75% on battery), let the screen sleep (it dims after 1 minute and turns off after 3), and charge it at your desk. The battery gauge and voltage are in the status bar and **menu → info**; noting the percentage before and after a work session tells you your real figure.
