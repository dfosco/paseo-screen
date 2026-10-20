Host screen shows the desktop of the Mac running your Paseo daemon and lets you control it with mouse and keyboard, from any Paseo client paired with that host: the desktop app, the web app or your phone. Frames and input travel over the existing Paseo connection, including the relay, so there is no extra port, VPN or account to set up.

## How it works

The daemon runs a small native helper. It captures the screen with ScreenCaptureKit as JPEG frames and posts mouse and keyboard events through Core Graphics. Frames are sized to your viewer, two requests stay in flight, and unchanged frames are skipped. Expect roughly 5 to 15 frames per second depending on the network. That is plenty to watch a browser test or click through a dialog, but it is not smooth video.

## Setup

The host must be a Mac running macOS 14 or later with the Xcode command line tools installed, because the helper is compiled on first use. Paseo plugins must be enabled on the host. Someone at the host Mac must give **Paseo** two permissions once in System Settings, Privacy and Security: Screen Recording (to see the screen) and Accessibility (to control it). Then quit and reopen Paseo on the host. The viewer shows which permission is missing and has a button that opens the macOS prompts on the host.

## Using it

Open **Screen** for the host from the sidebar, or search "host screen" in the command center.

- On desktop and web clients, click, drag, scroll and type directly on the picture. A drag that leaves the picture continues onto the host's neighbouring display.
- On phones, tap to click, double-tap to double-click, long-press to right-click, pinch to zoom and drag to pan. **Rotate** turns the picture sideways to use the whole phone screen.
- The key bar sends shortcuts your own computer keeps for itself: Cmd+Tab, Cmd+Space, Cmd+W, Cmd+Q, switching Spaces, Mission Control and App windows.
- Text clipboard works both ways on desktop and web clients. Cmd+V pastes your clipboard on the host, and Cmd+C on the host copies back to you. **Copy from host** and **Paste to host** buttons cover menu copies.
- With several displays, the display button names each one and switches between them. The last choice is remembered on the host.
- **View only** turns input off. **Fast, Balanced, Sharp** trade resolution for frame rate.

## Permissions and limits

Anyone paired with this Paseo host can see and control the host Mac while the plugin is enabled. That is the same trust that pairing already grants for running agents and terminals. Nothing leaves the Paseo connection. The plugin sends no audio, transfers no files and syncs text only, not images. Keys are sent by physical position, so both computers should use the same keyboard layout. Only Mac hosts are supported.
