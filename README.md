<div align="center">

# ClaudeStick

**A pocket-size handheld for Claude Code, made for being away from your desk.**<br>
Your sessions keep running at home or on a server. The Stick in your pocket, on your phone's hotspot, approves, steers and talks to them from anywhere.

[**Flash it**](https://soupscode.github.io/ClaudeStick-S3/) &nbsp;·&nbsp; [Set up remote access](docs/REMOTE.md) &nbsp;·&nbsp; [How to use](docs/HOW_TO_USE.md) &nbsp;·&nbsp; [Troubleshooting](docs/TROUBLESHOOTING.md)

![ClaudeStick screens](docs/screens.png)

</div>

Runs on the **M5Stack StickS3**. A small [bridge](docs/REMOTE.md) at home relays your Claude sessions over HTTPS; the Stick joins your phone's hotspot (or any Wi-Fi) and talks to the bridge. It also works directly on your home network, and with the Claude desktop app over Bluetooth.

> **Early release.** Feature-complete and covered by an automated test suite plus a real firmware build in CI, but still being validated on StickS3 hardware. [Open an issue](../../issues) if something misbehaves.

## What it does

- **Anywhere.** Approve and steer sessions from the train, the office or the park. It follows you across networks and tells you if it loses the link.
- **Approvals.** Every permission prompt lands on the Stick: allow, deny (even by voice, with a reason), or *always allow* with a rule that fits. Dangerous commands get a red card.
- **Live status.** One row per Claude session, with its to-do progress, context fill, 5-hour and 7-day plan limits, and a little buddy that reacts to what's happening.
- **Macros and questions.** One-button "Continue", "Next phase", "Run tests + fix", your own. Claude's multiple-choice questions become buttons.
- **Push-to-talk.** Hold a button, speak, confirm; the text goes to Claude.
- **Phone page and alerts.** The same controls in a web page, plus [ntfy](https://ntfy.sh) pushes with Approve / Deny buttons.
- **Stays current.** New versions arrive over Wi-Fi with a changelog, install in one press, and roll back on their own if a build won't boot.

![Eighteen buddies](docs/buddies.png)

## Install

**You need:** an M5Stack **StickS3** (K150) and a USB-C **data** cable; desktop **Chrome or Edge** for flashing; **Python 3** on an always-on machine for the bridge and on each machine where you run `claude`; a way to reach the bridge from outside (a domain with an HTTPS reverse proxy, or a VPN such as Tailscale); and a phone whose hotspot offers 2.4 GHz.

1. **Run the bridge** on a machine at home that stays on, then make it reachable over HTTPS ([how](docs/REMOTE.md)). It prints your token and the command for step 4.

   ```bash
   python3 bridge/claudestick_bridge.py
   ```

2. **Flash the Stick.** Open the [web flasher](https://soupscode.github.io/ClaudeStick-S3/) (a fork publishes its own at `https://<owner>.github.io/<repo>/`), click **Install** and accept *erase*, then **Connect to Wi-Fi** with your home network. No computer handy? A fresh Stick opens a hotspot called `ClaudeStick-XXXX`; join it from your phone.
3. **Set it up** on the page the flasher links to (`http://<stick-ip>:7880/setup`). Add your **phone's hotspot** under *wi-fi networks*, enter the **bridge's address and token**, pick *always on* for remote mode, and choose a name, buddy and volume. The Stick keeps up to four networks and joins the strongest one in range, so it follows you from home to the hotspot.
4. **Connect Claude Code.** Run the command the bridge printed on every machine where you use `claude`. It backs up `~/.claude/settings.json`, adds its hooks next to yours, keeps any status line you have, and is safe to re-run (`--test` checks the connection, `--uninstall` removes everything it added).

   ```bash
   curl -s http://<bridge-ip>:7890/install.py | python3 - --host <bridge-ip> --port 7890 --token <token>
   ```

5. **Claude desktop app** *(optional, only near your computer)*. Help → Troubleshooting → Enable Developer Mode → Developer → Open Hardware Buddy… → Connect, pick your Stick, enter the 6-digit code. Switch Bluetooth off in the Stick's settings to save battery when you're away.

Then trigger an approval in Claude Code. Only ever at your desk? Skip the bridge: the Stick's setup page prints a direct install command. No hardware yet? `python3 tools/mock_device.py --token test123` serves the Stick's API, phone page and setup wizard on your machine.

## Documentation

| | |
|---|---|
| [Remote access](docs/REMOTE.md) | The bridge: run it at home, reach it from anywhere, phone hotspot, what to expect, security |
| [How to use](docs/HOW_TO_USE.md) | Buttons, approvals, remote mode, sessions and usage, push-to-talk, phone page, settings, updates, battery |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Recovery, safe mode, serial commands, "you can't brick it" |
| [How it works](docs/HOW_IT_WORKS.md) | Architecture, hook events, HTTP API, updates and rollback |
| [Contributing](CONTRIBUTING.md) | Build, test, release, fork |
| [Security](SECURITY.md) | Trust model and reporting |

## Privacy

Nothing leaves your network except what you configure: update checks, voice audio and ntfy messages go only to the servers you set. The Stick and your machines talk over plain HTTP with a shared token, so keep the Stick on a network you trust; the [bridge](docs/REMOTE.md) adds HTTPS for use away from home. Details in [SECURITY.md](SECURITY.md).

## License and credits

MIT, see [LICENSE](LICENSE). `firmware/src/ble_bridge.*` and `firmware/src/buddies/*` come from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) (MIT); `tinyfont.h` is derived from the 3x5 "Tom Thumb" font (BSD 3-clause). The web flasher uses [ESP Web Tools](https://github.com/esphome/esp-web-tools). Built on [M5Unified](https://github.com/m5stack/M5Unified), [M5GFX](https://github.com/m5stack/M5GFX) and [ArduinoJson](https://arduinojson.org).
