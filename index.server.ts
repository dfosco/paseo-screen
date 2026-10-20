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
    const failed = { screenRecording: false, accessibility: false, locked: false, displays: [], preferredDisplay: null };
    if (process.platform !== "darwin") {
      return { ...failed, supported: false, error: "Screen sharing is only available on macOS hosts." };
    }
    try {
      const status = await capture.request<Pick<ScreenStatus, "screenRecording" | "accessibility" | "locked" | "displays">>(command);
      const preferred = await readPreferredDisplay();
      return {
        supported: true,
        error: null,
        screenRecording: status.screenRecording,
        accessibility: status.accessibility,
        locked: status.locked,
        displays: status.displays,
        preferredDisplay: status.displays.some((display) => display.id === preferred) ? preferred : null,
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
