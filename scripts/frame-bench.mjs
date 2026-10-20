// Measures frame RPC latency and size through a daemon connection.
// Usage: node scripts/frame-bench.mjs <extracted Paseo app.asar dir> [host target] [count]
import { pathToFileURL } from "node:url";
import path from "node:path";

const [asarDir, host, rawCount = "5"] = process.argv.slice(2);
const cli = (file) => import(pathToFileURL(path.join(asarDir, "node_modules/@getpaseo/cli/dist/utils", file)).href);
const { connectToDaemon } = await cli("client.js");
const { selectDaemonTarget } = await cli("daemon-target.js");
const client = await connectToDaemon({ target: selectDaemonTarget(host ? { host } : {}) });
const count = Number(rawCount);
try {
  for (const [maxWidth, quality] of [[1728, 0.6], [1170, 0.5], [780, 0.5]]) {
    const times = [];
    let bytes = 0;
    for (let i = 0; i < count; i++) {
      const started = performance.now();
      const frame = await client.invokePluginRpc("paseo-screen", "screen.frame", { maxWidth, quality });
      times.push(performance.now() - started);
      bytes = frame.bytes;
    }
    times.sort((a, b) => a - b);
    console.log(`${maxWidth}px q${quality}: ${Math.round(bytes / 1024)} KB, median ${Math.round(times[times.length >> 1])} ms, min ${Math.round(times[0])} ms`);
  }
  const pings = [];
  for (let i = 0; i < count; i++) {
    const started = performance.now();
    await client.invokePluginRpc("paseo-screen", "screen.clipboard", {});
    pings.push(performance.now() - started);
  }
  pings.sort((a, b) => a - b);
  console.log(`tiny RPC round trip: median ${Math.round(pings[pings.length >> 1])} ms`);
} finally {
  await client.close();
}
