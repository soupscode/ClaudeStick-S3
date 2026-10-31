# Security

## Model

ClaudeStick is meant for a **trusted home or lab network**.

- The Stick and the hooks talk over **plain HTTP with a shared token** (`X-Token`). Anyone who can read your network traffic can read the token. Put the Stick on a trusted network or VPN (for example Tailscale) for remote use.
- **Away from home** the optional bridge ([docs/REMOTE.md](docs/REMOTE.md)) is what you expose, not the Stick. It uses the same shared token, so serve it over **HTTPS** (a reverse proxy with a real certificate, `--tls-cert`, or a VPN). It rate-limits wrong tokens per address (ten in a minute lock that address out for five minutes), caps request sizes and open connections, keeps its secrets (voice key, ntfy token) away from the settings page and the Stick, and never logs the token. The phone link carries the token in its URL, so it shows up in proxy logs and browser history; rotate with `--rotate-token` if it leaks. The Stick verifies the bridge's certificate against its embedded Mozilla root list (`http://` bridge addresses are for a VPN or LAN and send the token in clear text).
- **Setup is open until it is finished:** `/setup` and its API are reachable without a token so the wizard can show you the token (including on the setup hotspot). Finish setup promptly. Afterwards everything needs the token.
- **Firmware updates are not code-signed.** The Stick trusts the image served by its update source over HTTPS, verified against an embedded Mozilla root bundle. Whoever controls the repo or site behind that URL controls what your Stick runs, so protect them (2FA, branch protection).
- Anyone with the token can approve tool calls on your behalf. Treat it like a password; **menu → new token** rotates it (re-run the install command afterwards).
- The danger-card patterns and *always allow* rules are heuristics to help you notice risky actions, not a security boundary.

## Reporting a vulnerability

Please report it privately through GitHub: the repository's **Security → Report a vulnerability** page. Don't open a public issue for it. Include the firmware version, what you found and how to reproduce it.
