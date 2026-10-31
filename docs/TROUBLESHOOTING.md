# Troubleshooting and recovery

- **No serial port in the flasher:** try another cable; it must carry data.
- **Flashing fails:** hold the side button until the green LED blinks (download mode), then retry.
- **Upside down:** menu → *flip screen*. If tilt scrolls the wrong way, run `tilt flip` in the flasher's *Logs & Console*.
- **Title says `claudestick:safe`:** Bluetooth was switched off because the Stick crashed repeatedly on boot. Open *Logs & Console* and run `info`; the "last reset" line says why. Include it in a bug report.
- **A machine can't reach the Stick:** re-run the install command with `--test`; it says whether the Stick answers and whether the token is accepted. Check that both are on the same network. If the IP changed, the hook finds the Stick again by its device id at the next prompt.
- **Serial commands** (*Logs & Console* or any serial monitor): `info`, `wifi <ssid> <pass>`, `token <t>`, `name <n>`, `tilt on|off|flip`, `flip`, `tz <POSIX rule>`, `clock 12|24`, `font 0|1`, `bat` (battery level, voltage and drain), `sleep max|min` (experimental radio sleep), `setup`, `forget`, `reboot`.
- **Hold B at power-on** starts in safe mode (no Bluetooth); **hold A+B at power-on** forgets Wi-Fi and reruns setup.
- **You can't brick it with firmware.** The ESP32-S3's first-stage bootloader is in ROM, so download mode (hold the side button while plugging in USB) always works. From there, flash again from the web flasher, `pio run -t upload`, or erase with `esptool.py --chip esp32s3 erase_flash`. This firmware never touches eFuses (secure boot or flash encryption), the only way to lock an ESP32 permanently.
- **Away from home:** the bridge's error messages and what they mean are in [REMOTE.md](REMOTE.md#troubleshooting).
- **Still stuck?** [Open an issue](../../../issues) with the firmware version (menu → info) and the "last reset" line.
