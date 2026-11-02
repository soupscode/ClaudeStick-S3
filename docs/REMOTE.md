# Remote access: the bridge and your hotspot

This is how ClaudeStick is meant to be used. Your Claude Code sessions run at home (or on a server); you are out with the Stick on your phone's hotspot, approving, answering and steering them. Your home network isn't reachable from there, so a small program, the **bridge**, runs at home and relays:

```
 home                                                              away
 Claude Code --hooks--> bridge (:7890) <--HTTPS--> your router / reverse proxy <--HTTPS-- Stick (on a phone hotspot)
 (any machine)           keeps the sessions,                                              or the phone page
                         approvals and cards
```

The hooks talk to the bridge exactly like they talk to a Stick. The Stick (or the phone page) polls the bridge every second or two, shows what is waiting, and sends your answers back. At home it works the same way, so nothing changes when you leave. Only ever at your desk? You don't need any of this: see [How to use](HOW_TO_USE.md) for direct mode.

> **Status:** the bridge and its protocol are covered by automated tests (including the real hook running against it). The Stick-side client is tested in the firmware logic harness but has **not been run on a StickS3 yet**; see [Known limits](#known-limits).

## 1. Run the bridge at home

On a computer that stays on and can be reached by the machines where you run `claude` (it can be one of them). Python 3.8+, standard library only.

```bash
git clone <this repository's URL> claudestick && cd claudestick
python3 bridge/claudestick_bridge.py
```

The first run creates `~/.claudestick-bridge/` (private to you) with a random token and prints it together with the install command. `--show-token` prints them again, `--rotate-token` makes a new token, `--data DIR` moves the folder. To keep it running, use a service. A systemd example is in [`bridge/claudestick-bridge.service`](../bridge/claudestick-bridge.service) (edit the user and paths, then `systemctl enable --now claudestick-bridge`).

**Pairing codes.** To set up a device without typing the long token, ask for a one-time code: `python3 bridge/claudestick_bridge.py --pair` (while the bridge runs) or the **pair a device** card on its settings page. A code is 6 digits, works once, lasts 5 minutes and dies after 3 wrong tries. The Stick, the phone page and the installer each trade it for the token themselves, so the token is never typed or pasted.

Its settings page is `http://<bridge-ip>:7890/setup?t=<token>`: macros, approval waits, voice, ntfy and its **public address** (step 3). The page also shows the install command and the phone link.

## 2. Point Claude Code at the bridge

On every machine where you run `claude`, run the command the bridge printed (or the setup page shows):

```bash
curl -s http://<bridge-ip>:7890/install.py | python3 - --host <bridge-ip> --port 7890 --token <token>
```

With a pairing code instead of the token, use `--pair 123456` (and for a bridge on the internet, give its full address: `--host https://stick.example.com --pair 123456`). The code is spent on the first try and not saved anywhere; the installer stores the token it got back, in the same private file as before.

This is the same installer as for a Stick, so it's safe to re-run and `--uninstall` still works. If a machine was installed against a Stick before, this simply points it at the bridge. Hooks and the bridge speak plain HTTP on your home network; they never need the public address.

## 3. Make it reachable (reverse proxy + port forward)

The Stick needs an HTTPS address for the bridge. The usual setup:

1. A DNS name for your home (a dynamic-DNS name works), e.g. `stick.example.com`.
2. A reverse proxy at home that terminates TLS with a real certificate (Let's Encrypt) and forwards to the bridge.
3. A port forward on your router: TCP 443 to the proxy.

Run the bridge with `--trust-proxy` so its per-address lockout sees real clients rather than the proxy, and bind it to the proxy only if they share a machine (`--host 127.0.0.1`).

**Caddy** (gets the certificate for you):

```
stick.example.com {
    reverse_proxy 127.0.0.1:7890
}
```

**nginx** (certificate from certbot or your own):

```nginx
server {
    listen 443 ssl;
    server_name stick.example.com;
    # ssl_certificate / ssl_certificate_key ...
    client_max_body_size 5m;                      # voice clips go to /stt
    location / {
        proxy_pass http://127.0.0.1:7890;
        proxy_http_version 1.1;
        proxy_set_header Connection "";           # keep-alive to the bridge
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_read_timeout 70s;
    }
}
```

These are examples, not tested configurations; adapt them to your proxy. Then open `https://stick.example.com/health`: it should answer `{"ok": true, ... "bridge": true}`. Put the same address in the bridge's settings page under **public address**: the phone link and the buttons in ntfy alerts use it.

No proxy at all? `--tls-cert fullchain.pem --tls-key privkey.pem` serves HTTPS directly (TLS 1.2+). A VPN such as Tailscale or WireGuard is the safer alternative to opening a port: put the Stick's phone or hotspot on it and use `http://<vpn-address>:7890` as the bridge address.

## 4. Set up the Stick

The Stick's setup page (the one the flasher opens, or `http://<stick-ip>:7880/setup?t=<token>` later; do this at home) asks for everything:

- **wi-fi networks:** add your **phone's hotspot** (name and password). The Stick keeps up to four networks and joins the strongest one it can see, so it follows you from home to the hotspot. The Stick is 2.4 GHz only: make sure the hotspot offers 2.4 GHz (many phones have a "maximize compatibility" or band setting for this). WPA2 personal is what it speaks.
- **remote access (bridge):** the bridge's public address (`https://stick.example.com`) and either its token or a fresh **pairing code**, then **connect**. The page tells you if the address, the token, the code or the connection is wrong.
- **remote mode:** *always on* is right for a device you carry (see *What to expect*).
- **Bluetooth:** switch it off if you won't be near your computer; it saves battery.

A small cloud in the status bar shows the link: green connected, red can't reach it, grey still connecting. The info screen says why.

Your phone can use the bridge too: open the bridge's address and enter a pairing code, or use the phone link the setup page shows (`https://stick.example.com/?t=<token>`). It works without a Stick at all.

## What to expect

- **Speed and battery.** The Stick checks the bridge every 1.5 s with the screen on and every 4 s when dimmed, so a new approval shows up within a couple of seconds. The battery is small (250 mAh): see [Battery](HOW_TO_USE.md#battery) for what to expect, how the Stick measures it, and what helps (a power bank, Bluetooth off).
- **Remote mode.** The bridge's default is **always on**, which only ever applies while a Stick or phone page is connected: prompts wait up to 10 minutes for you, and Claude is told you're on a small device. Switch the Stick off or leave it out of range and the bridge sees nobody connected, so prompts wait 20 s and then appear in the terminal as if the Stick weren't there; they never hang on a Stick that's switched off. If you keep the Stick on at your desk and want the terminal to take over when you type, choose *auto* (bridge settings page, the Stick's menu or the phone page).
- **Losing the connection.** After two failed checks the Stick tells you (a notice and a tone, because Claude may be waiting on you), drops everything from the bridge (the prompts have timed out at home by then) and retries with a growing delay, up to a minute. When it's back it says so and shows the current state quietly, without chiming for old news.
- **Voice.** Push-to-talk sends the recording to the bridge, which forwards it to the speech server in *its* settings (so the Stick never needs to reach it, and the API key stays at home).
- **Macros and menu.** For a bridge session the menu lists the bridge's macros; the Done card options come from the bridge too.
- **Clock.** HTTPS needs the date. The Stick gets it over NTP; a hotspot normally allows that. Until then it says *waiting for the clock*.
- **Several machines.** Any number can use one bridge; sessions from all of them appear together.
- **Direct and bridge together.** Machines pointed at the Stick directly keep working and appear alongside bridge sessions.

## Security

Anyone with the token can approve what Claude runs on your machines. Treat the token like an SSH key.

- 128-bit random token, compared in constant time. Ten wrong tokens from one address lock it out for five minutes (`429`).
- Pairing codes are the only thing besides the pages that works without the token. A code is made by someone who already has the token, is single-use, lasts 5 minutes, and is replaced when a new one is made. Three wrong tries kill it; five wrong tries from one address lock that address out of pairing for five minutes. Someone who can reach the bridge can burn a live code by guessing (you just make another), but 6 digits and these limits make guessing one hopeless. Like typing the token, the exchange is only private over HTTPS.
- There is one token for everything: you can't revoke a single device. If a phone is lost or a machine is retired, rotate with `--rotate-token` and pair the rest again.
- Everything except `/health`, the pages and the installer needs the token. Request sizes and open connections are capped.
- Use HTTPS on anything reachable from the internet. The Stick verifies the certificate against its embedded Mozilla root list; `http://` addresses are for a VPN or LAN and send the token in clear text.
- The phone link carries the token in the URL, so it appears in your proxy's access logs and your browser history. Rotate with `--rotate-token` if it leaks, then re-run the install command on each machine and re-enter the token on the Stick and phone.
- The bridge hides secrets (voice key, ntfy token) from the settings page and from the Stick; only the hooks can ask for them.
- Keep the bridge and proxy patched, and think before exposing any home service to the internet. A VPN avoids that entirely.

See also [SECURITY.md](../SECURITY.md).

## Known limits

- The Stick-side client hasn't been run on hardware. The things most likely to need adjusting are TLS memory use (a connection holds roughly 20 KB of RAM; the Stick refuses to start one with less than 32 KB free and lets it go if memory gets tight) and how long a reconnect blocks the screen (up to a few seconds, with a growing back-off while the bridge is unreachable).
- Prompts are not queued while the Stick is offline: the hook gives up after about 15 s and the terminal asks instead.
- The bridge keeps its state in memory. Restarting it forgets open prompts (the hooks fall back to the terminal) and sessions reappear with their next event.
- WPA2-Enterprise networks (some offices) aren't supported; use a hotspot.
- Captive portals (hotel or cafe Wi-Fi that wants a login page) block the Stick.

## Troubleshooting

| You see | Meaning |
|---|---|
| Red cloud, `token rejected` | The token on the Stick doesn't match the bridge's (`--show-token`). |
| `waiting for the clock` | No internet time yet (NTP blocked?). It retries by itself. |
| `low memory` | Not enough free RAM for TLS right now; restart the Stick. |
| `not a ClaudeStick bridge` | The address answers but isn't the bridge (wrong port or proxy path). |
| `too many attempts, waiting` | Your address was locked out for five minutes after repeated wrong tokens. |
| `that pairing code is wrong or has expired` | Codes work once, last 5 minutes and die after 3 wrong tries. Make a new one (`--pair`, or the bridge's setup page). |
| Settings page says *the Stick isn't on Wi-Fi yet* | Connect it to Wi-Fi first, then save the bridge address. |
| A Claude Code session isn't listed | Sessions load their hooks when they start: restart ones that were open when you installed. `claudestick_bridge.py --sessions` shows who has reported in. |
| `claude` asks in the terminal instead of the Stick | Remote mode is off or nothing is connected to the bridge; see *Remote mode* above. |

`curl -s https://stick.example.com/health` and `python3 install.py --test` check each hop.

## The bridge's API

It speaks the Stick's API (see [HOW_IT_WORKS.md](HOW_IT_WORKS.md)) plus a few additions: `GET /state?v=<n>` answers `{"v":n,"same":true}` when nothing changed (`n=1` asks for the Stick's trimmed view), every state has a version `v`, an epoch `now` and per-session `plan_done_n`; `POST /stt` takes a WAV and returns `{"text": ...}`; `GET /config?secrets=1` is for hooks. Tests: `tests/py/e2e_bridge.py`, `tests/py/e2e_bridge_hooks.py` and `tests/py/contract_bridge.py`, which checks the bridge answers hooks exactly like the Stick's firmware does.
