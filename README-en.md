# obs-delay-stream  v5.0.1

**VRChat Dancer/Music Performer Support Tool [obs-delay-stream]**

![Windows](https://img.shields.io/badge/platform-Windows-blue)
![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue)
![OSS: OBS Studio](https://img.shields.io/badge/OSS-OBS%20Studio-lightgrey)
![OSS: WebSocket++](https://img.shields.io/badge/OSS-WebSocket++-lightgrey)
![OSS: FFmpeg](https://img.shields.io/badge/OSS-FFmpeg-lightgrey)

[BOOTH](https://mz1987records.booth.pm/items/8134637) | [Report a Bug](https://github.com/MZ1987Records/obs-delay-stream/issues/new/choose) | [日本語](README.md)

<p align="center">
  <img src="receiver/images/obs-delay-stream-logo.svg" alt="obs-delay-stream logo" width="400">
</p>

An OBS plugin that automatically measures and resolves sync drift between dancers and between dancer/world music — using only OBS and Google Chrome. It adds a WebSocket audio streaming feature to OBS for performers.
Supports up to 20 simultaneous dancer/performer connections.

Each performer's individual delay is automatically measured, and a time-corrected audio stream is delivered to their Google Chrome browser for perfectly synchronized playback.
Also includes delay-sync adjustment for audio streamed to the VRChat world.
Built-in IP-hiding tunnel support.

- No SYNCROOM or DAW required.
- Dancers/performers simply open the receiver URL (provided by the streamer) in Google Chrome to receive low-latency, synchronized audio.
- Does not affect the VRChat client.
- The receiver page supports volume control, re-sync / auto re-sync, and JP/EN language display.

---

## Settings Overview

| Section | Description |
|------|------|
| Per-Performer Channel Settings | Manage each performer's name. In advanced edit mode, you can adjust individual sync settings. |
| Stream ID / IP | Stream ID and host IP are configured automatically. In advanced mode, you can set the host IP manually. |
| WebSocket | Configure audio codec, start/stop the streaming server, and control transmission. |
| Tunnel | Generate a public URL via cloudflared. Share externally without exposing your IP directly. |
| URL Distribution | Copy all performer URLs at once. Reduces the hassle and risk of distribution errors. |
| Sync Flow | Measure and apply delays for performers and RTMP in two steps. Follow the on-screen guide to align overall timing. |
| Master / RTMP | Manually adjust master delay and apply RTMP measurement results. Fine-tune drift including the streaming path. |
| Global Offset | Add a common offset to all channels. Correct any remaining perceived timing difference across the board. |

---

## Installation

1. Download and extract the latest `obs-delay-stream-vX.X.X.zip` from [Releases](https://github.com/MZ1987Records/obs-delay-stream/releases) or [BOOTH](https://mz1987records.booth.pm/items/8134637).
2. The ZIP contains two options: `For ProgramData` and `For Program Files (legacy)`. Choose the one that matches your OBS installation.

### ProgramData Installation (Recommended)

1. Place `For ProgramData/plugins/obs-delay-stream` into:

```
C:\ProgramData\obs-studio\plugins\
```

2. Restart OBS Studio.

### Program Files Installation (Legacy)

1. Place `For Program Files (legacy)/obs-plugins/64bit/obs-delay-stream.dll` into:

```
C:\Program Files\obs-studio\obs-plugins\64bit\
```

2. Place `For Program Files (legacy)/data/obs-plugins/obs-delay-stream` into:

```
C:\Program Files\obs-studio\data\obs-plugins\
```

3. If existing files are present, overwriting them is fine (for updates).
4. Restart OBS Studio (administrator privileges may be required).

### Verify Installation

1. Launch OBS Studio.
2. Right-click an audio source (microphone, desktop audio, etc.).
3. Go to **Filters** → **+** → select **"obs-delay-stream"**.
4. If the GUI panel opens, installation was successful.

---

## Usage

### Initial Setup

1. Open the filter panel.
2. Enter each performer's name under **Per-Performer Channel Settings**.
3. Click the **Start WebSocket Server** button.

### Using the Tunnel (IP Hiding — Recommended)

1. Leave `cloudflared.exe path` as `auto` (only enter the exe path if you want to specify a custom location).
2. Click the **Start Tunnel** button (the exe will be downloaded automatically on first use by default).
3. A URL in the format `https://xxxx.trycloudflare.com` will be generated.

> **Note:** Security software may block `*.trycloudflare.com` and cause tunnel connection failures.
> If this happens, add `*.trycloudflare.com` as an exception (allowed).

The auto-downloaded exe is saved to:
`%LOCALAPPDATA%\obs-delay-stream\bin\cloudflared.exe`

### Sharing Connection Info with Performers

Click the **Copy All Performer URLs** button and paste into Discord or similar. Have each performer open their corresponding URL.

### Sync Flow (Recommended Procedure)

1. Confirm that all performers are connected to the receiver page.
2. Click the **Start Sync Flow** button.
3. Step 1: After automatic measurement completes, confirm that the base delay for each channel has been applied automatically.
4. Step 2: After RTMP measurement completes, review the master delay and click **Apply and Finish**.

---

## Developer Information

For build instructions, troubleshooting, and file structure, see [BUILDING.md](BUILDING.md).

---

## License

- [GNU General Public License v2.0 or later](LICENSE)
- For third-party licenses, see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
