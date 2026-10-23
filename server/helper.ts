import { execCommand, spawnProcess, terminateProcess } from "@getpaseo/plugin/server";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, readdir, rename, rm, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import { HELPERS } from "./helper-source.generated";

const REQUEST_TIMEOUT_MS = 15_000;
/** After a failed build, wait this long before trying again, so status polling does not retry constantly. */
const BUILD_RETRY_MS = 60_000;
const CACHE_ROOT =
  process.platform === "darwin"
    ? path.join(homedir(), "Library", "Caches", "paseo-screen")
    : process.platform === "win32"
      ? path.join(process.env.LOCALAPPDATA || path.join(homedir(), "AppData", "Local"), "paseo-screen")
      : path.join(process.env.XDG_CACHE_HOME || path.join(homedir(), ".cache"), "paseo-screen");
/**
 * The helper for this host: Swift with ScreenCaptureKit on macOS, C# with GDI on Windows, and C on Linux,
 * built either for X11 or, with -DPASEO_WAYLAND, for the Wayland desktop portals.
 */
const HELPER = process.platform === "linux" ? HELPERS.linux : process.platform === "win32" ? HELPERS.win32 : HELPERS.darwin;
const LINUX_PACKAGES = [
  "Debian or Ubuntu: sudo apt install gcc libx11-dev libxext-dev libxtst-dev libxrandr-dev libxfixes-dev libjpeg-dev.",
  "Fedora: sudo dnf install gcc libX11-devel libXext-devel libXtst-devel libXrandr-devel libXfixes-devel libjpeg-turbo-devel.",
  "Arch: sudo pacman -S gcc libx11 libxext libxtst libxrandr libxfixes libjpeg-turbo.",
].join(" ");
const WAYLAND_PACKAGES = [
  "Debian or Ubuntu: sudo apt install gcc pkg-config libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libjpeg-dev gstreamer1.0-pipewire.",
  "Fedora: sudo dnf install gcc pkgconf-pkg-config glib2-devel gstreamer1-devel gstreamer1-plugins-base-devel libjpeg-turbo-devel pipewire-gstreamer.",
  "Arch: sudo pacman -S gcc pkgconf glib2 gstreamer gst-plugins-base libjpeg-turbo gst-plugin-pipewire.",
].join(" ");

export type LinuxBackend = "x11" | "wayland";

export const NO_SESSION =
  "No desktop session to share on this host: nobody is logged in to a Wayland desktop, and the Paseo daemon has no X display (DISPLAY is not set). Log in on the host, or start Paseo from the desktop session.";

/**
 * Which Linux helper this daemon needs. The environment answers for a daemon started inside the
 * desktop session. A daemon started outside it (a systemd user service, or over SSH) asks logind
 * about the user's graphical session instead: a Wayland session is reachable through the session
 * bus, while an X11 one also needs DISPLAY, which only the environment can give. Only an answer is
 * remembered, so logging in after the daemon started is picked up.
 */
let knownBackend: LinuxBackend | null = null;
export async function linuxBackend(): Promise<LinuxBackend | null> {
  if (knownBackend) return knownBackend;
  const session = process.env.XDG_SESSION_TYPE;
  if (session === "wayland" || (session !== "x11" && process.env.WAYLAND_DISPLAY)) knownBackend = "wayland";
  else if (process.env.DISPLAY) knownBackend = "x11";
  else if ((await graphicalSessionType()) === "wayland") knownBackend = "wayland";
  return knownBackend;
}

async function graphicalSessionType(): Promise<string | null> {
  try {
    const uid = String(process.getuid?.() ?? "");
    const display = (await execCommand("loginctl", ["show-user", uid, "-p", "Display", "--value"], { timeout: 5_000 })).stdout.trim();
    if (!display) return null;
    return (await execCommand("loginctl", ["show-session", display, "-p", "Type", "--value"], { timeout: 5_000 })).stdout.trim();
  } catch {
    return null;
  }
}

/** The portals live on the user's session bus; a daemon started outside the session may lack its address. */
function helperEnvironment(): NodeJS.ProcessEnv | undefined {
  if (process.platform !== "linux" || knownBackend !== "wayland" || process.env.DBUS_SESSION_BUS_ADDRESS) return undefined;
  const bus = `/run/user/${process.getuid?.()}/bus`;
  return existsSync(bus) ? { ...process.env, DBUS_SESSION_BUS_ADDRESS: `unix:path=${bus}` } : undefined;
}

/** Compiles the embedded helper once per source revision (and Linux backend) and returns the binary path. */
let buildPromise: Promise<string> | null = null;
let lastFailure: { at: number; error: unknown } | null = null;
export function ensureHelperBinary(): Promise<string> {
  if (!buildPromise && lastFailure && Date.now() - lastFailure.at < BUILD_RETRY_MS) return Promise.reject(lastFailure.error);
  buildPromise ??= buildHelper().then(
    (binary) => {
      lastFailure = null;
      return binary;
    },
    (error: unknown) => {
      buildPromise = null;
      // A missing session is not a build failure: retry as soon as someone logs in.
      if (!(error instanceof Error && error.message === NO_SESSION)) lastFailure = { at: Date.now(), error };
      throw error;
    },
  );
  return buildPromise;
}

/** Cache folder name: the source hash, with a suffix for the Wayland build of the Linux helper. */
function buildName(backend: LinuxBackend | null): string {
  return backend === "wayland" ? `${HELPER.hash}-wayland` : HELPER.hash;
}

async function buildHelper(): Promise<string> {
  const backend = process.platform === "linux" ? await linuxBackend() : null;
  // Without a session there is nothing to build for; the build is tried again once one appears.
  if (process.platform === "linux" && !backend) throw new Error(NO_SESSION);
  const dir = path.join(CACHE_ROOT, buildName(backend));
  // Windows only runs files with an executable extension.
  const binary = path.join(dir, process.platform === "win32" ? "paseo-screen-helper.exe" : "paseo-screen-helper");
  if (existsSync(binary)) return binary;
  await mkdir(dir, { recursive: true });
  const source = path.join(dir, HELPER.file);
  const partial = `${binary}.partial`;
  await writeFile(source, HELPER.source);
  if (process.platform === "linux") await compileLinuxHelper(source, partial, backend);
  else if (process.platform === "win32") await compileWindowsHelper(source, partial);
  else await compileMacHelper(source, partial);
  await renameWithRetry(partial, binary);
  // Best effort: an old helper that is still running, or a virus scan, can hold its files on Windows.
  await removeOldBuilds().catch(() => undefined);
  return binary;
}

async function compileLinuxHelper(source: string, output: string, backend: LinuxBackend | null): Promise<void> {
  if (backend === "wayland") return compileWaylandHelper(source, output);
  const libraries = ["-lX11", "-lXext", "-lXtst", "-lXrandr", "-lXfixes", "-ljpeg", "-lpthread", "-lm"];
  try {
    await execCommand(process.env.CC || "cc", ["-O2", "-std=gnu11", "-o", output, source, ...libraries], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = buildFailure(error);
    throw new Error(`Could not build the screen helper. It needs a C compiler and the X11 and JPEG development packages. ${LINUX_PACKAGES} ${detail}`);
  }
}

async function compileWaylandHelper(source: string, output: string): Promise<void> {
  const modules = ["gio-unix-2.0", "gstreamer-app-1.0", "gstreamer-video-1.0"];
  try {
    const flags = (await execCommand("pkg-config", ["--cflags", "--libs", ...modules], { timeout: 30_000 })).stdout.trim().split(/\s+/);
    await execCommand(process.env.CC || "cc", ["-O2", "-std=gnu11", "-DPASEO_WAYLAND", "-o", output, source, ...flags, "-ljpeg", "-lpthread", "-lm"], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = buildFailure(error);
    throw new Error(
      `Could not build the screen helper for Wayland. It needs a C compiler, pkg-config, and the GLib, GStreamer and JPEG development packages. ${WAYLAND_PACKAGES} ${detail}`,
    );
  }
}

/**
 * Builds the Windows helper with the C# compiler that ships with the .NET Framework 4.x in every
 * Windows 10 and 11, so nothing has to be installed.
 */
async function compileWindowsHelper(source: string, output: string): Promise<void> {
  const windows = process.env.WINDIR || process.env.SystemRoot || "C:\\Windows";
  const candidates = ["Framework64", "Framework"].map((folder) => path.join(windows, "Microsoft.NET", folder, "v4.0.30319", "csc.exe"));
  const compiler = candidates.find((candidate) => existsSync(candidate));
  if (!compiler) {
    throw new Error("Could not find the .NET Framework C# compiler (csc.exe) that Windows normally includes, so the screen helper cannot be built.");
  }
  const references = ["System.dll", "System.Drawing.dll", "System.Windows.Forms.dll", "System.Web.Extensions.dll"].map((dll) => `/reference:${dll}`);
  try {
    await execCommand(compiler, ["/nologo", "/optimize+", "/target:exe", "/platform:anycpu", `/out:${output}`, ...references, source], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = buildFailure(error);
    throw new Error(`Could not build the screen helper with ${compiler}. ${detail}`);
  }
}

async function compileMacHelper(source: string, output: string): Promise<void> {
  // `xcode-select -p` only reports; calling xcrun without the tools would pop up the macOS install prompt.
  try {
    await execCommand("/usr/bin/xcode-select", ["-p"], { timeout: 10_000 });
  } catch {
    throw new Error("The Xcode command line tools are needed to build the screen helper. Run `xcode-select --install` on the host, then reopen the screen.");
  }
  try {
    await execCommand("xcrun", ["swiftc", "-O", "-swift-version", "5", "-o", output, source], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = buildFailure(error);
    throw new Error(`Could not build the screen helper. Install the Xcode command line tools (xcode-select --install). ${detail}`);
  }
}

/** Renames, retrying briefly while Windows holds the new file (for example during a virus scan). */
async function renameWithRetry(from: string, to: string): Promise<void> {
  for (let attempt = 0; ; attempt++) {
    try {
      await rename(from, to);
      return;
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (attempt >= 10 || (code !== "EBUSY" && code !== "EPERM" && code !== "EACCES")) throw error;
      await new Promise((resolve) => setTimeout(resolve, 300));
    }
  }
}

/** A failed compiler run's message, with its output: csc.exe reports errors on stdout, not stderr. */
function buildFailure(error: unknown): string {
  const message = error instanceof Error ? error.message : String(error);
  const stdout = typeof (error as { stdout?: unknown }).stdout === "string" ? ((error as { stdout: string }).stdout).trim() : "";
  return stdout && !message.includes(stdout) ? `${message}\n${stdout.slice(-2000)}` : message;
}

/** Deletes helpers built from earlier source revisions. */
async function removeOldBuilds(): Promise<void> {
  const entries = await readdir(CACHE_ROOT).catch(() => [] as string[]);
  await Promise.all(
    entries.filter((name) => name !== buildName(null) && name !== buildName("wayland")).map((name) => rm(path.join(CACHE_ROOT, name), { recursive: true, force: true })),
  );
}

type Reply = Record<string, unknown> & { ok: boolean; error?: string };

/** One long-lived helper process speaking line-delimited JSON. Restarts on the next request after it exits. */
export class HelperProcess {
  private child: ChildProcessWithoutNullStreams | null = null;
  private starting: Promise<ChildProcessWithoutNullStreams> | null = null;
  private nextId = 1;
  /** Set by stop(); a stopped helper never starts again, even if a build finishes afterwards. */
  private stopped = false;
  private pending = new Map<number, { resolve: (reply: Reply) => void; reject: (error: Error) => void; timer: NodeJS.Timeout }>();

  constructor(private readonly log: (message: string) => void) {}

  async request<T extends Record<string, unknown>>(command: string, payload: Record<string, unknown> = {}): Promise<T> {
    const child = await this.start();
    const id = this.nextId++;
    const reply = await new Promise<Reply>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`screen helper timed out on ${command}`));
      }, REQUEST_TIMEOUT_MS);
      this.pending.set(id, { resolve, reject, timer });
      child.stdin.write(`${JSON.stringify({ ...payload, id, cmd: command })}\n`);
    });
    if (!reply.ok) throw new Error(reply.error ?? `screen helper failed on ${command}`);
    return reply as unknown as T;
  }

  async stop(): Promise<void> {
    this.stopped = true;
    const child = this.child;
    this.child = null;
    this.failPending(new Error("screen helper stopped"));
    if (child) await terminateProcess(child);
  }

  private start(): Promise<ChildProcessWithoutNullStreams> {
    if (this.stopped) return Promise.reject(new Error("screen helper stopped"));
    if (this.child) return Promise.resolve(this.child);
    this.starting ??= ensureHelperBinary()
      .then((binary) => {
        if (this.stopped) throw new Error("screen helper stopped");
        const child = spawnProcess(binary, [], { stdio: "pipe", env: helperEnvironment() });
        child.on("exit", (code, signal) => {
          if (this.child === child) this.child = null;
          this.failPending(new Error(`screen helper exited (${signal ?? code})`));
        });
        child.on("error", (error) => this.log(`screen helper error: ${error.message}`));
        child.stdin.on("error", (error) => this.log(`screen helper stdin error: ${error.message}`));
        createInterface({ input: child.stderr }).on("line", (line) => this.log(`helper: ${line}`));
        createInterface({ input: child.stdout }).on("line", (line) => this.onLine(line));
        this.child = child;
        return child;
      })
      .finally(() => {
        this.starting = null;
      });
    return this.starting;
  }

  private onLine(line: string): void {
    let reply: Reply & { id?: number };
    try {
      reply = JSON.parse(line);
    } catch {
      this.log(`helper sent invalid JSON: ${line.slice(0, 200)}`);
      return;
    }
    const entry = typeof reply.id === "number" ? this.pending.get(reply.id) : undefined;
    if (!entry) return;
    this.pending.delete(reply.id as number);
    clearTimeout(entry.timer);
    entry.resolve(reply);
  }

  private failPending(error: Error): void {
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(error);
    }
    this.pending.clear();
  }
}
