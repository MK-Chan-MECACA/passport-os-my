<div align="center">

# Passport OS MY

**A small device worth keeping in your daily routine.**

Malaysia edition of the Passport OS community firmware for the FoloToy AI Passport · ESP32-C3 · 8 MB Flash · 12 apps

**Ask Xiaozhi to adjust the volume, play Malaysian radio, read tech news and set reminders on the device.**

[Voice control](#ask-xiaozhi-to-operate-the-device) · [Apps](#twelve-apps) · [Install](#install) · [Make your own version](#make-your-own-version) · [Related projects](#related-projects)

</div>

---

## What this edition changes

Passport OS MY is a fork of [Passport OS](https://github.com/manchunx7-bit/passport-os) 0.2.0-rc.59 for English-speaking users in Malaysia. Everything not listed here works as upstream.

- **English-only interface.** All on-screen text, voice tools and setup pages are in English. Fonts are Latin-only, which makes the app image about 500 KB smaller.
- **Malaysian cities.** City search uses GeoNames places in Malaysia, grouped by state or Federal Territory. Kuala Lumpur is the default. The phone page still accepts a custom name and coordinates.
- **Malaysian radio.** Use ↑ and ↓ to browse local stations, built-in Malaysian presets and the full Malaysian directory of supported MP3 and AAC/AAC+ streams. The built-in stations also work when the directory is unavailable.
- **Tech News instead of AI Hot.** Xiaozhi reads Hacker News front-page stories and recent AI stories.
- **NetEase Cloud Music removed.** Asking Xiaozhi to play music plays live radio instead.
- **Unchanged on purpose:** Malaysia time (UTC+8), Open-Meteo weather, the official xiaozhi.me backend, and the `Passport-WiFi-xxxx` setup hotspot.

## Ask Xiaozhi to operate the device

**Xiaozhi is the device's voice control entry point.** Double-press OK to start a conversation, then use everyday language to adjust settings, open apps, play radio or manage tasks.

| Example request | Device action |
| --- | --- |
| "Turn the volume down" or "Set screen brightness to 50%" | Adjust and save the device's volume or brightness |
| "Play BFM in Kuala Lumpur and stop in half an hour" | Find the station, open Radio to play it, and set a stop timer |
| "Play some radio from Penang" or "Play some music" | Play a popular station for that place; music requests are answered with live radio |
| "What's on Hacker News?" or "Any news about Nvidia?" | Read front-page stories, or recent popular stories matching a word (AI by default) |
| "Start a Pomodoro" or "How much time is left?" | Start a 25-minute focus session or read the actual remaining time; pause and resume are also supported |
| "Remind me to drink water in ten minutes" or "Remind me to read every day at 9 pm" | Save a one-time or recurring reminder with an on-device alert and chime |
| "Create a packing list: keys, phone, umbrella" or "Mark umbrella as done" | Save a checklist, then read and update its items and completion state |
| "Open Stopwatch" or "Go to Home" | Open the requested app or screen |

Robot expressions and an audio waveform follow listening and speech, longer text expands into a reading area, and all four themes are supported.

Voice tools need a network connection and a Xiaozhi device binding. **On [xiaozhi.me](https://xiaozhi.me), set your agent's language and voice to English**, otherwise replies come back in Chinese. The Xiaozhi servers are in China, so expect a slower voice round trip from Malaysia. Radio depends on the online directory. Opening another app or starting playback ends the current Xiaozhi conversation; double-press OK again for further voice control. The device must stay powered on for reminders to trigger.

## Twelve apps

| App | What it does | Requirements |
| --- | --- | --- |
| Home | Clock, profile, portraits, cards, weather and air quality | Network for weather and air quality |
| Radio | Malaysian city and station selection, playback and sleep timer | Wi-Fi and available online stations |
| AI Voice | Hardware microphone and configurable desktop hotkeys | Windows companion program |
| Xiaozhi AI | Voice conversations, volume and brightness, app opening, radio requests, tech news, timers, reminders and lists | Network and a Xiaozhi binding |
| Reminders | One-time and recurring reminders, chimes and snooze | Works offline; uses the device clock |
| Lists | View, check off and manage tasks | Works offline |
| Muse | Send voice messages and read replies on screen | Phone pairing, your own account and a working network |
| Why Barking | A sound-controlled game | Works offline |
| Sound Jump | Longer sounds jump farther; stop making sound to jump | Works offline |
| Tetris | Stack blocks and clear lines | Works offline |
| Stopwatch | Timing, splits, pause and resume | Works offline |
| Pomodoro | Focus sessions, breaks and completion chimes | Works offline |

The device also has themes, volume, brightness, auto screen-off, wake on short press, phone-based Wi-Fi setup, battery reporting and power management.

## Install

Prebuilt images are on the [GitHub Releases page](https://github.com/MK-Chan-MECACA/passport-os-my/releases). You do not need AI tools or a firmware development environment to install them.

Use only the matching FoloToy AI Passport hardware revision with ESP32-C3 and 8 MB Flash. Similar-looking boards may have incompatible displays, audio chips, buttons or partition layouts.

| Situation | Image | Address |
| --- | --- | --- |
| First installation or a fresh start | `Passport-OS-MY-vX.Y.Z-full.bin` | `0x0` |
| Existing Passport OS with a verified identical partition layout | `Passport-OS-MY-vX.Y.Z-app.bin` | `0x10000` |

> **Back up first.** A full image overwrites configuration in its written range, and a complete reset also needs a flash erase. To keep Wi-Fi, profile and images, verify the partition layout and write only the application image, without erasing the whole flash.

Step-by-step flashing commands and common problems are in the [install guide](docs/install.md).

<details>
<summary><strong>First boot: connect your phone</strong></summary>

1. Connect your phone to the device's password-free `Passport-WiFi-xxxx` hotspot.
2. Open `http://192.168.4.1` in the browser.
3. Choose Wi-Fi, your city and your profile details. Portraits and cards are optional.
4. Save, then return to Home.

Xiaozhi and Muse need your own service setup. For Muse through a proxy, enter the proxy host's LAN IP and HTTP port. AI Voice also needs the Windows companion from the related projects below.

</details>

## Make your own version

**This repository contains the complete device firmware.** Download the source, change apps or the interface, build an image and test it on matching hardware.

| Area | Source |
| --- | --- |
| Application UI and logic | [`main/apps/`](main/apps/) |
| Launcher, settings and app lifecycle | [`main/launcher/`](main/launcher/) |
| Xiaozhi device control and media handover | [`main/device_control_mcp.cc`](main/device_control_mcp.cc) · [`main/voice_intent.cc`](main/voice_intent.cc) |
| Shared UI and fonts | [`main/ui/`](main/ui/) · [`main/fonts/`](main/fonts/) |
| Board drivers | [`components/bsp/`](components/bsp/) |
| Wi-Fi setup | [`components/esp-wifi-connect/`](components/esp-wifi-connect/) |
| Muse integration | [`components/passport_muse/`](components/passport_muse/) |
| Tests and development tools | [`tests/`](tests/) · [`tools/`](tools/) |

### Build

Install Git, ESP-IDF 5.5.3 and its toolchain. On Windows you can build in WSL and flash through a Windows serial port.

```bash
git clone https://github.com/MK-Chan-MECACA/passport-os-my.git
cd passport-os-my
bash build.sh release
```

The script loads ESP-IDF from `$IDF_PATH`, or `/opt/esp-idf` if that is not set. For another installation path:

```bash
IDF_DIR="$HOME/esp/esp-idf" bash build.sh release
```

ESP-IDF Component Manager downloads the declared dependencies. The build checks distribution defaults, including public mode and empty proxy defaults.

| File | Use |
| --- | --- |
| `build-public/FoloToy-AI-Passport-full.bin` | Full image, written at `0x0` |
| `build-public/passport-os.bin` | Application image, written at `0x10000` only for a verified matching partition layout |

### Verify

With ESP-IDF 5.5.3 activated:

```bash
bash tools/validate.sh --static
bash tools/validate.sh --firmware
```

The second command builds and verifies a full image at `build/FoloToy-AI-Passport-full.bin`. A successful build does not replace testing buttons, display, audio, networking and the behaviour you changed on real hardware.

<details>
<summary><strong>Packaging your own release</strong></summary>

The local packaging tool bundles a source snapshot, a manifest and SHA256 checksums:

```bash
python3 tools/package_firmware.py --date YYYY-MM-DD --verification /path/to/verification-public.json
```

Replace the date and verification path with real values. The verification record must match the exact firmware version and contain no personal data, and release notes must exist at `docs/release-<version>.md`. Packaging does not publish anything. Pushing a tag builds the firmware in GitHub Actions and attaches the `-full.bin`, `-app.bin` and `SHA256.txt` files to a GitHub Release.

</details>

## Privacy

Public builds enable `PASSPORT_PUBLIC_RELEASE=ON` and clear proxy defaults. Each user enters their own Wi-Fi, accounts and service credentials on their own device.

Do not commit or attach passwords, API tokens, device exports, private logs or personal images. When reporting a problem, include the firmware version, hardware revision, steps to reproduce and whether old data was kept.

## Related projects

- [Muse: ai-passport-muse](https://github.com/manchunx7-bit/ai-passport-muse): standalone module source and usage guide.
- [AI Voice: folo-ai-passport-voice](https://github.com/manchunx7-bit/folo-ai-passport-voice): voice application source, Windows companion and usage guide.

## Documentation

- [Install guide](docs/install.md)
- [Current status](docs/CURRENT.md) and [maintenance](docs/MAINTENANCE.md)
- [Malaysia edition plan](docs/MY-EDITION.md)
- [Xiaozhi app notes](docs/xiaozhi-app.md)
- Upstream reviews, plans and release notes, kept in Chinese: [`docs/archive/zh/`](docs/archive/zh/)

## Licence and credits

The main project uses the [MIT licence](LICENSE). Muse, the Noise protocol code and other third-party content keep their own licences and notices. Jollybot artwork has [separate terms](main/apps/muse/JOLLYBOT-NOTICE.md). The city catalogue uses [GeoNames](third_party/geonames-my/README.md) data under CC BY 4.0.

Thanks to FoloToy for the hardware and upstream project, to the Passport OS author for the community firmware, and to everyone who shares ideas, reports issues and contributes improvements.

Malaysia edition maintained by [MK-Chan-MECACA](https://github.com/MK-Chan-MECACA).
