import { execCommand, spawnProcess, terminateProcess } from "@getpaseo/plugin/server";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, rename, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import { HELPER_SOURCE, HELPER_SOURCE_HASH } from "./helper-source.generated";

const REQUEST_TIMEOUT_MS = 15_000;

/** Compiles the embedded Swift helper once per source revision and returns the binary path. */
let buildPromise: Promise<string> | null = null;
export function ensureHelperBinary(): Promise<string> {
  buildPromise ??= buildHelper().catch((error) => {
    buildPromise = null;
    throw error;
  });
  return buildPromise;
}

async function buildHelper(): Promise<string> {
  const dir = path.join(homedir(), "Library", "Caches", "paseo-screen", HELPER_SOURCE_HASH);
  const binary = path.join(dir, "paseo-screen-helper");
  if (existsSync(binary)) return binary;
  await mkdir(dir, { recursive: true });
  const source = path.join(dir, "main.swift");
  const partial = `${binary}.partial`;
  await writeFile(source, HELPER_SOURCE);
  try {
    await execCommand("xcrun", ["swiftc", "-O", "-swift-version", "5", "-o", partial, source], {
      timeout: 300_000,
      maxBuffer: 4 * 1024 * 1024,
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(`Could not build the screen helper. Install the Xcode command line tools (xcode-select --install). ${detail}`);
  }
  await rename(partial, binary);
  return binary;
}

type Reply = Record<string, unknown> & { ok: boolean; error?: string };

/** One long-lived helper process speaking line-delimited JSON. Restarts on the next request after it exits. */
export class HelperProcess {
  private child: ChildProcessWithoutNullStreams | null = null;
  private starting: Promise<ChildProcessWithoutNullStreams> | null = null;
  private nextId = 1;
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
    const child = this.child;
    this.child = null;
    this.failPending(new Error("screen helper stopped"));
    if (child) await terminateProcess(child);
  }

  private start(): Promise<ChildProcessWithoutNullStreams> {
    if (this.child) return Promise.resolve(this.child);
    this.starting ??= ensureHelperBinary()
      .then((binary) => {
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
