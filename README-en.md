# obs-delay-stream  v8.0.0

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

An OBS plugin that automatically measures and resolves sync drift between dancers and between dancer/world music — using only OBS and Google Chrome. It adds WebSocket audio streaming to OBS for performers.
Supports up to 20 simultaneous performer connections.

Each performer's individual delay is automatically measured, and a time-corrected audio stream is delivered to their Google Chrome browser for perfectly synchronized playback.
Also supports OBS broadcast (RTSP E2E) latency measurement for synchronization with audio streamed to the VRChat world.
Built-in IP-hiding tunnel support.

- No SYNCROOM or DAW required.
- Performers simply open the receiver URL (provided by the streamer) in Google Chrome to receive low-latency, synchronized audio.
- Does not affect the VRChat client.
- The receiver page supports volume control, re-sync / auto re-sync, spectrum meter, and JP/EN language display.

---

## Settings Tabs

| Tab | Description |
|------|------|
| Performer Names | Manage each performer's name. Add, remove, and reorder channels. |
| Tunnel | Generate a public URL via cloudflared. Share externally without exposing your IP directly. |
| WS Streaming | Configure audio codec (Opus / PCM), start/stop the WebSocket server, and control transmission. |
| URL Sharing | View and copy all performer URLs at once. Reduces the hassle and risk of distribution errors. |
| WS Measurement | Auto-measure WebSocket delivery latency for all connected performers. Also supports auto-measurement on performer connect. |
| RTSP Measurement | Measure OBS broadcast (RTSP E2E) latency. Choose between Silent and Mix measurement modes. |
| Fine Tune | Per-channel environment latency settings and overall delay diagram. Also configure estimated avatar latency here. |

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
2. Enter each performer's name under the **Performer Names** tab.
3. Go to the **WS Streaming** tab and click **Start WebSocket Server**.

### Using the Tunnel (IP Hiding — Recommended)

1. Leave `cloudflared.exe path` as `auto` (only enter the exe path if you want to specify a custom location).
2. Click the **Start Tunnel** button (the exe will be downloaded automatically on first use by default).
3. A URL in the format `https://xxxx.trycloudflare.com` will be generated.

> **Note:** Security software may block `*.trycloudflare.com` and cause tunnel connection failures.
> If this happens, add `*.trycloudflare.com` as an exception (allowed).

The auto-downloaded exe is saved to:
`%LOCALAPPDATA%\obs-delay-stream\bin\cloudflared.exe`

### Sharing Connection Info with Performers

In the **URL Sharing** tab, click **Copy performer URL list** and paste into Discord or similar. Have each performer open their corresponding URL in Google Chrome.

### Latency Measurement (Recommended Procedure)

1. Confirm that all performers are connected to the receiver page.
2. In the **WS Measurement** tab, measure all connected channels (auto-measurement on connect is also available).
3. After measurement completes, base delays are automatically applied to each channel.
4. In the **RTSP Measurement** tab, measure OBS broadcast latency (start OBS streaming beforehand).
5. If needed, use the **Fine Tune** tab to adjust environment latency and avatar latency.

---

## Developer Information

For build instructions, troubleshooting, and file structure, see [BUILDING.md](BUILDING.md).

---

## License

- [GNU General Public License v2.0 or later](LICENSE)
- For third-party licenses, see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
