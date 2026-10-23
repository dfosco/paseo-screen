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
    : path.join(process.env.XDG_CACHE_HOME || path.join(homedir(), ".cache"), "paseo-screen");
/** The helper for this host: Swift with ScreenCaptureKit on macOS, C with X11 on Linux. */
const HELPER = process.platform === "linux" ? HELPERS.linux : HELPERS.darwin;
const LINUX_PACKAGES = [
  "Debian or Ubuntu: sudo apt install gcc libx11-dev libxext-dev libxtst-dev libxrandr-dev libxfixes-dev libjpeg-dev.",
  "Fedora: sudo dnf install gcc libX11-devel libXext-devel libXtst-devel libXrandr-devel libXfixes-devel libjpeg-turbo-devel.",
  "Arch: sudo pacman -S gcc libx11 libxext libxtst libxrandr libxfixes libjpeg-turbo.",
].join(" ");

/** Compiles the embedded helper once per source revision and returns the binary path. */
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
      lastFailure = { at: Date.now(), error };
      throw error;
    },
  );
  return buildPromise;
}

async function buildHelper(): Promise<string> {
  const dir = path.join(CACHE_ROOT, HELPER.hash);
  const binary = path.join(dir, "paseo-screen-helper");
  if (existsSync(binary)) return binary;
  await mkdir(dir, { recursive: true });
  const source = path.join(dir, HELPER.file);
  const partial = `${binary}.partial`;
  await writeFile(source, HELPER.source);
  if (process.platform === "linux") await compileLinuxHelper(source, partial);
  else await compileMacHelper(source, partial);
  await rename(partial, binary);
  await removeOldBuilds();
  return binary;
}

async function compileLinuxHelper(source: string, output: string): Promise<void> {
  const libraries = ["-lX11", "-lXext", "-lXtst", "-lXrandr", "-lXfixes", "-ljpeg", "-lpthread", "-lm"];
  try {
    await execCommand(process.env.CC || "cc", ["-O2", "-std=gnu11", "-o", output, source, ...libraries], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(`Could not build the screen helper. It needs a C compiler and the X11 and JPEG development packages. ${LINUX_PACKAGES} ${detail}`);
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
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(`Could not build the screen helper. Install the Xcode command line tools (xcode-select --install). ${detail}`);
  }
}

/** Deletes helpers built from earlier source revisions. */
async function removeOldBuilds(): Promise<void> {
  const entries = await readdir(CACHE_ROOT).catch(() => [] as string[]);
  await Promise.all(
    entries.filter((name) => name !== HELPER.hash).map((name) => rm(path.join(CACHE_ROOT, name), { recursive: true, force: true })),
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
        const child = spawnProcess(binary, [], { stdio: "pipe" });
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
