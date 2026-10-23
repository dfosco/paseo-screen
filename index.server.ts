import type { PluginServerContext } from "@getpaseo/plugin/server";
import { HelperProcess } from "./server/helper";
import { readPreferredDisplay, writePreferredDisplay } from "./server/preferences";
import { chooseDisplayRpc, clipboardRpc, frameRpc, inputRpc, requestPermissionsRpc, statusRpc, type ScreenStatus } from "./shared/rpc";

export default function contribute(server: PluginServerContext) {
  const log = (message: string) => console.error(`[paseo-screen] ${message}`);
  // Separate processes so clicks and keys never queue behind a frame capture.
  const capture = new HelperProcess(log);
  const input = new HelperProcess(log);

  async function readStatus(command: "status" | "request"): Promise<ScreenStatus> {
    const failed = { platform: process.platform, screenRecording: false, accessibility: false, locked: false, displays: [], preferredDisplay: null };
    const unsupported = unsupportedReason();
    if (unsupported) return { ...failed, supported: false, error: unsupported };
    try {
      const status = await capture.request<
        Pick<ScreenStatus, "screenRecording" | "accessibility" | "locked" | "displays"> & { activeDisplay?: number }
      >(command);
      let preferred = await readPreferredDisplay();
      const connected = (id: number | null) => status.displays.some((display) => display.id === id);
      // With no choice saved yet, start where the focused window is (Linux helpers report it), and save
      // that, so the viewer does not follow focus around afterwards.
      if (preferred === null && status.activeDisplay !== undefined && connected(status.activeDisplay)) {
        preferred = status.activeDisplay;
        await writePreferredDisplay(preferred).catch(() => undefined);
      }
      return {
        supported: true,
        error: null,
        platform: process.platform,
        screenRecording: status.screenRecording,
        accessibility: status.accessibility,
        locked: status.locked,
        displays: status.displays,
        preferredDisplay: connected(preferred) ? preferred : null,
      };
    } catch (error) {
      return { ...failed, supported: true, error: error instanceof Error ? error.message : String(error) };
    }
  }

  server.handle(statusRpc, () => readStatus("status"));
  server.handle(requestPermissionsRpc, () => readStatus("request"));
  server.handle(frameRpc, (request) => capture.request("frame", request));
  server.handle(inputRpc, (request) => input.request("input", request));
  // Read on the input helper so it runs after any copy shortcut queued before it.
  server.handle(clipboardRpc, () => input.request("clipboard"));
  server.handle(chooseDisplayRpc, async ({ display }) => {
    await writePreferredDisplay(display);
    return {};
  });

  return () => {
    void capture.stop();
    void input.stop();
  };
}

/** Why this host cannot share its screen, or null when it can. */
function unsupportedReason(): string | null {
  // Windows support is experimental: built and reviewed, not yet tested on a real Windows host.
  if (process.platform === "darwin" || process.platform === "win32") return null;
  if (process.platform !== "linux") return "Screen sharing is only available on macOS, Linux and Windows hosts.";
  // Through Xwayland, an X11 capture only sees X11 windows, and input does not reach Wayland apps.
  // WAYLAND_DISPLAY also covers compositors started without a login manager, which may leave
  // XDG_SESSION_TYPE unset (for example Sway or Hyprland launched from a TTY).
  const session = process.env.XDG_SESSION_TYPE;
  if (session === "wayland" || (session !== "x11" && process.env.WAYLAND_DISPLAY)) {
    return "This Linux host is running a Wayland session, and screen sharing needs X11 for now. If your login screen offers an X11 or Xorg session, log out and choose it; recent GNOME releases no longer include one.";
  }
  if (!process.env.DISPLAY) {
    return "The Paseo daemon on this host has no X display (DISPLAY is not set). Start Paseo from the desktop session.";
  }
  return null;
}
