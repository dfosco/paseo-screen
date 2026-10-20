# Host screen for Paseo

A [Paseo](https://paseo.sh) plugin that lets you see and control the screen of the Mac running your Paseo daemon, from any paired Paseo client: the desktop app, the web app or your phone. Think of a small TeamViewer that rides on the Paseo connection you already have, relay included, so there is no port forwarding, VPN, VNC or extra account.

It was built for the moments when an agent on a remote Mac needs a human: watching a Playwright run, clicking through a permission dialog, or checking what is actually on screen.

![Controlling the host Mac from Paseo on a laptop](images/laptop-client.jpg)

## Features

- Live view of the host's screen, sized to your viewer, at roughly 5 to 15 frames per second depending on the network.
- Mouse and keyboard control: click, double-click, right-click, drag, scroll and type.
- A key bar for shortcuts your own computer keeps for itself: ⌘Tab, ⌘Space, ⌘W, ⌘Q, switching Spaces (⌃←/⌃→), Mission Control and App windows.
- Two-way text clipboard on desktop and web clients. ⌘V pastes your clipboard on the host, and ⌘C on the host copies back to you.
- Multiple displays: each one is listed by name, the last choice is remembered, and a drag that leaves the picture continues onto the neighbouring display.
- Phones: tap to click, long-press to right-click, pinch to zoom, drag to pan, and a Rotate button that turns the picture sideways.
- Wakes the host's display when you open the viewer, and shows the lock screen if the Mac is locked so you can type your password to unlock it.
- View-only mode, and Fast / Balanced / Sharp quality presets.

![Viewing the host's second display](images/desktop-viewer.jpg)

![Typing and selecting text on the host from another computer](images/remote-editing.jpg)

## Requirements

- A Mac host running macOS 14 or later, with the Xcode command line tools (`xcode-select --install`). The native helper is compiled with `swiftc` the first time it is used.
- Paseo 0.12 or later, with plugins enabled on the host.
- Any Paseo client paired with that host.

## Install

1. Enable plugins on the host by setting `"pluginsEnabled": true` in `~/.paseo/config.json`, then run `paseo reload`.
2. Install the plugin on the host:

   ```bash
   paseo plugin install npm:@kevlai22/paseo-screen
   # or from GitHub
   paseo plugin install git:kevguy/paseo-screen
   ```

3. On the host Mac, open System Settings → Privacy & Security and turn on **Paseo** under both **Screen Recording** and **Accessibility**. Quit and reopen Paseo.
4. In any Paseo client, open **Screen · _host name_** from the sidebar, or search "host screen" in the command center.

If a permission is missing, the viewer says which one and offers a button that shows the macOS prompt on the host. Someone has to be at the host Mac to approve it once.

macOS gives these permissions to the app that started the Paseo daemon. That is normally the Paseo desktop app. If you run the daemon some other way, for example `paseo start` from a terminal, grant the permissions to that app (such as Terminal) instead.

## How it works

```
Paseo client (laptop, web, phone)                    Host Mac (Paseo daemon)
  Host screen view  ── plugin RPC over Paseo relay ──▶  plugin server
   - polls screen.frame, two in flight                   ├─ helper #1: ScreenCaptureKit → JPEG
   - sends screen.input batches                          └─ helper #2: CGEvent mouse and keyboard
```

- `helper/main.swift` is a small native helper speaking line-delimited JSON. The plugin server embeds its source, compiles it into `~/Library/Caches/paseo-screen/<hash>/`, and runs two copies so input never waits behind a frame capture.
- macOS grants Screen Recording and Accessibility to Paseo as the responsible app, so rebuilding the helper does not need new permissions.
- Frames are JPEGs sized to the viewer. Unchanged frames come back without image data.
- The RPCs are defined in `shared/rpc.ts`: `screen.status`, `screen.request-permissions`, `screen.frame`, `screen.input`, `screen.clipboard` and `screen.choose-display`.

## Security

Anyone paired with the host can see and control its screen while the plugin is enabled. That is the same trust that pairing already grants for running agents and terminals on that machine. Traffic stays inside the Paseo connection; the plugin opens no ports and talks to no other service. View-only mode is a setting in each viewer, not a lock on the host: any paired client can still send input. Turn the plugin off with `paseo plugin disable paseo-screen` when you don't need it.

## Limitations

- Only Mac hosts are supported. Any Paseo client can view.
- It is frame polling, not video. Fast motion looks choppy, and the frame rate depends on your link to the Paseo relay.
- Granting the two macOS permissions needs someone physically at the host once.
- The plugin doesn't stop the host from sleeping or locking on its own schedule. Opening the viewer, using the mouse or keyboard on the picture, or the **Wake display** button wakes the display, and you unlock it by typing your password. Touch ID isn't available remotely.
- Shortcuts your own computer or the Paseo app handle first (⌘Tab, ⌘Space, ⌘W, ⌘Q) need the key bar buttons.
- Clipboard sync is text only, and only on desktop and web clients.
- Keys are sent by physical position, so both computers should use the same keyboard layout.
- No audio and no file transfer.

## Development

```bash
npm install
npm run typecheck   # also re-embeds helper/main.swift into server/helper-source.generated.ts
npm test            # rotate/zoom/pan math for the touch viewer
paseo plugin install "$PWD"           # once, from a checkout
paseo plugin reload paseo-screen      # after changes
paseo plugin logs paseo-screen
```

After editing `helper/main.swift`, run `npm run embed` (or `npm run typecheck`). The plugin bundle runs from memory, so the server can't read files next to itself and the helper source has to be embedded.

`scripts/` also has tools that call the plugin through a running daemon, using Paseo's CLI internals from an extracted copy of the Paseo app (`npx @electron/asar extract /Applications/Paseo.app/Contents/Resources/app.asar <dir>`):

- `rpc-probe.mjs <asar dir> <method> [json]` calls one RPC and saves frames to `build/probe-frame.jpg`.
- `frame-bench.mjs` and `pipeline-bench.mjs` measure frame latency and throughput, locally or through the relay.
- `compile-check.mjs` bundles the plugin with Paseo's own plugin compiler.

## License

[MIT](LICENSE)
