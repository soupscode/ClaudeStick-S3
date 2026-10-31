# Telemetry (opt-in)

Off by default, and hidden from setup entirely until a destination is configured. Turning it on sends a small, periodic usage report — never anything you typed, any tool, command, approval or macro text, voice audio, or a token, SSID or URL you configured. This page lists exactly what a report contains, then how to run your own collector in about a minute.

## 1. Run the collector

Any machine that can receive the Stick's reports. Python 3.8+, standard library only.

```bash
git clone <this repository's URL> claudestick && cd claudestick
python3 telemetry/claudestick_telemetry.py
```

The first run creates `~/.claudestick-telemetry/` (private to you: SQLite database + a random `/stats` token) and starts listening on port 7895. `--data DIR` moves the folder, `--show-token` prints the `/stats` token again, `--summary` prints aggregate numbers straight from the database without starting a server at all. To keep it running, use a service: a systemd example is in [`telemetry/claudestick-telemetry.service`](../telemetry/claudestick-telemetry.service) (edit the user and paths, then `systemctl enable --now claudestick-telemetry`).

```bash
python3 telemetry/claudestick_telemetry.py --summary
#   reports:        128
#   devices:        14
#   active (7 d):   9
#   firmware versions:
#     0.9.2      14
#   totals across every report on file:
#     approvals            612
#     macro_taps           88
#     ...
```

## 2. Point a Stick at it

Settings page → **you & your stick** → *send anonymous usage statistics* (only shown once a destination is set below), and **firmware** → *telemetry destination*: `http://<collector-ip>:7895` on your home network, or an HTTPS address if you've exposed it (next section). Each Stick decides for itself, opt-in; nothing is sent until both the checkbox and the destination are set.

Reports go out roughly every 6 hours (first one ~90 seconds after boot), only when idle (never delays an approval), and only while the Stick has Wi-Fi. **A Stick used mainly away from home** ([REMOTE.md](REMOTE.md)) reports from wherever it currently is, so the collector needs to be reachable from there too, same as the bridge — see below. A Stick that's only ever on your home LAN can point at a bare `http://` address and skip all of that.

## 3. Make it reachable from anywhere (optional)

Exactly the same shape as [exposing the bridge](REMOTE.md#3-make-it-reachable-reverse-proxy--port-forward): a reverse proxy with a real certificate, forwarding to the collector. If you're already running a bridge, add the collector as a second site on the same proxy (a different subdomain or port both work).

**Caddy:**

```
stats.example.com {
    reverse_proxy 127.0.0.1:7895
}
```

**nginx:**

```nginx
server {
    listen 443 ssl;
    server_name stats.example.com;
    location / {
        proxy_pass http://127.0.0.1:7895;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

Run the collector with `--trust-proxy` so its rate limiter sees real client addresses instead of the proxy's. These are examples, not tested configurations; adapt them to your proxy.

## What a report contains

Sent as one JSON `POST /report`, roughly every 6 hours:

| Field | What it is |
|---|---|
| `device_id` | Random, generated on first use. Not your Wi-Fi MAC and not the id used for mDNS/discovery. |
| `fw` | Firmware version. |
| `uptime_s`, `boot_epoch` | How long it's been running; when it last booted (only if the clock is known). |
| `reset_reason`, `safe_mode` | Why it last restarted (power on / crash / watchdog / ...), whether Bluetooth is off because of a crash loop. |
| `approvals`, `denies`, `always_allow`, `danger_cards` | Counts since the last report: how many tool calls were allowed, denied, allow-rule'd, or shown as a danger card. Never which tool, command or project. |
| `macro_taps`, `done_choose`, `done_dismiss` | How many times a macro was tapped, a Done/Question card answered, or dismissed. Never which macro or what it said. |
| `voice_ok`, `voice_fail` | Push-to-talk attempts that transcribed successfully vs. failed. Never the audio or the text. |
| `sessions_seen` | How many distinct Claude Code sessions this Stick has seen since the last report. |
| `remote_on_s`, `remote_off_s` | Seconds spent in remote mode on vs. off. |
| `battery_pct`, `battery_charging`, `battery_drain_pct_h` | What the info screen's `batt` row shows. |
| `bridge_configured`, `bt_on`, `remote_mode`, `text_style`, `buddy`, `tilt_on`, `flip_on`, `wifi_networks` | Feature flags and settings choices — on/off and which option, never the bridge's address or your Wi-Fi networks' names. |

That's the complete list: the collector drops any field it doesn't recognize (`telemetry/claudestick_telemetry.py`'s `FIELDS`), so even a modified Stick can't make it store anything else. The source for what gets counted and sent is `firmware/src/telemetry.inc` and the call sites listed at the top of that file.

## Security

- **No token on `/report`.** Keeping telemetry opt-in-only with nothing else to configure means the endpoint is open-write, like ntfy's. The collector bounds the damage: a strict field allowlist (unknown keys dropped, wrong types dropped, strings clipped, numbers clamped), a 4 KB body cap, and a per-address rate limit (20 reports / 10 minutes — far above one real device's cadence).
- **`/stats` is token-gated.** A random token is generated on first run (`--show-token`), compared in constant time.
- **Use HTTPS** if the collector is reachable from the internet, same reasoning as the bridge: plain `http://` sends everything, including the rate-limiter's view of who's making requests, in clear text. It's not sensitive the way an approval or a voice clip is, but there's no reason to skip it when a reverse proxy is one config block away.
- **Check what a build was baked with:** CI's release check (`tools/verify_release.py`) fails the build if `CS_TELEMETRY_URL` is set but isn't inside the firmware image, and its "release" notice on each run says either `telemetry destination: <url>` or that none was baked in.
- Nothing here is obfuscated or hidden: the opt-in checkbox, the field list, the collector's source and its schema are all in this repo. A fork's own build reports nowhere by default (`CS_TELEMETRY_URL` is empty unless its maintainer sets it), and anyone can point their own Stick at their own collector, or none at all, regardless of what a build was baked with — see the *telemetry destination* field in settings.
