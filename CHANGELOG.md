# Changelog

Each version's bullets are shown on the Stick's "update available!" prompt.

## 0.9.9
- The Stick explains what a bridge address is doing: a redirect (use https://), a blocked request (403) and a proxy that can't reach the bridge (502/503/504) instead of "error 301"

## 0.9.8
- Menus move with a quick flick of the wrist instead of a tilt: slow or held tilts are ignored, one flick is one step, and there's a wide angle where nothing registers

## 0.9.7
- Reinstall the firmware on purpose if it keeps crashing: menu > reinstall firmware (hold A), or "reinstall" on the setup page. It works even on the version you already have; settings are kept
- After a crash the Stick says so and points at the reinstall; "check for updates" offers a skipped or rolled-back version again
- Sessions are named the way Claude Code names them (/rename, or its own title) instead of folder@branch
- `claudestick_bridge.py --sessions` lists the Claude Code sessions the bridge has heard from, and how long ago

## 0.9.6
- Voice and macro messages are queued, and the Stick offers "send now": the message goes in before Claude's next tool call
- A message for a session that isn't working (turn ended, background tasks running) can wake it: the installer adds a background Stop hook (--wake / --no-wake)
- Sessions that are only waiting stay listed instead of dropping off after 30 minutes
- Tilt back/forward moves up/down in menus and Done cards (a tap on the power button switches the Stick off, so it no longer does anything)
- Done and Question cards say "hold B" dismisses (or answers at the keyboard)

## 0.9.5
- Wake the screen when you pick the Stick up (setting: "wake the screen when picked up")
- A message waiting for Claude's next step shows on the Stick and phone page, and can be cancelled
- Fix: voice, macros and "stop now" went to an arbitrary bridge session; they now go to the one that was active last
- Voice review says when the session is idle (it sees the message on its next step)

## 0.9.4
- Fix: the Stick shows the task Claude started most recently, not the oldest one still marked in progress
- Fix: a long task list follows the current task instead of starting at the top

## 0.9.3
- Pair with a 6-digit code instead of the long token: Stick, phone page and installer (`--pair`)
- The bridge makes codes from its setup page or `claudestick_bridge.py --pair`; each works once for 5 minutes
- Hooks and the installer can now reach a bridge by its https address

## 0.9.2
- Opt-in telemetry: off by default, hidden until a destination is set, see docs/TELEMETRY.md
- Self-hosted telemetry collector (telemetry/) with a one-command quickstart
- Settings: telemetry checkbox and destination; nothing sent until both are set

## 0.9.1
- Built for being away from your desk: setup asks for your hotspot and bridge right away
- Remote mode "always on" is the bridge's default, only while your Stick is connected
- The Stick tells you when it loses the bridge, and when it's back
- Battery: drain meter on the info screen, Bluetooth can be switched off, experimental radio sleep
- Bridge settings page has the remote mode setting

## 0.9.0
- Away from home: follow your home Claude sessions from any Wi-Fi through a bridge (docs/REMOTE.md)
- Remembers up to four Wi-Fi networks (home, a phone hotspot) and joins the strongest
- Voice, phone page and alert buttons work through the bridge
- Fix: tying the Stick to a session that then ended skipped other sessions' prompts
- Tests live in the repo and gate publishing in CI; release-ready docs

## 0.8.1
- Update checks wait for the clock and for a quiet moment
- Clear message when memory is too low to update
- Runs at 160 MHz (less battery); restarts itself if the heap gets badly fragmented
- More tamper-proof token check

## 0.8.0
- New layout: full-width content, small buddy face in the corner
- Big buddy on the idle screen and in the buddy picker (menu)
- Usage strip: context %, 5-hour and 7-day plan limits (Pro/Max)
- Installer wraps your status line to feed the Stick (--no-statusline to skip)
- Phone page shows limits and context %

## 0.7.2
- Clock shows 12-hour time (8:59p); 24-hour optional
- Time zone step in setup, detected from your browser
- Clock sets itself over the internet and follows daylight saving
- Wi-Fi bars now smoothed (no flicker)
- New "code" text style (slashed zero, cleaner punctuation)

## 0.7.1
- New status bar: Wi-Fi bars, Bluetooth, battery gauge, clock
- Key-cap hints along the bottom
- Spinner and elapsed time on working sessions
- Shell-style command cards ($ prompt, wrapped at / _ - =)
- Red border on danger cards
- Smart quotes, accents and emoji show as plain text

## 0.7.0
- Session details (tap A on Home): to-do list + tokens
- Home shows to-do progress per session
- Double-press A = always allow (and allows DANGER cards)
- Hold A on an approval: deny + tell Claude why
- Phone: to-do list, token use, Deny + note
- Chime (+ phone push when remote) when the plan is done
- Hold B on an approval = answer at keyboard

## 0.6.1
- Hardware test screen (menu)
- Flip screen option (menu, settings, serial)
- Safe mode: Bluetooth off if a fresh flash keeps crashing (or hold B at power-on)
- Shows why the Stick last restarted

## 0.6.0
- Update prompt on the Stick, with changelog
- Auto rollback if an update won't boot
- Hooks update themselves to match the Stick
- "check for updates" in the menu

## 0.5.0
- Push-to-talk (hold A) via your speech-to-text server
- Always-allow (hold A on approvals)
- Red DANGER cards for destructive commands
- Diffs on edit approvals
- Auto remote mode
- Settings stored on the Stick
- Wi-Fi updates from the settings page
- Finds the Stick again if its IP changes
