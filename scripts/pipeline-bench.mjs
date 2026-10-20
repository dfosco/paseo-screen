// Frames per second through a daemon connection with 1 or 2 frame requests in flight.
// Usage: node scripts/pipeline-bench.mjs <extracted Paseo app.asar dir> [host target]
import { pathToFileURL } from "node:url";
import path from "node:path";

const [asarDir, host] = process.argv.slice(2);
const cli = (file) => import(pathToFileURL(path.join(asarDir, "node_modules/@getpaseo/cli/dist/utils", file)).href);
const { connectToDaemon } = await cli("client.js");
const { selectDaemonTarget } = await cli("daemon-target.js");
const client = await connectToDaemon({ target: selectDaemonTarget(host ? { host } : {}) });
try {
  for (const maxWidth of [1728, 800]) {
    for (const lanes of [1, 2]) {
      let replies = 0;
      let kb = 0;
      const until = Date.now() + 5000;
      await Promise.all(Array.from({ length: lanes }, async () => {
        while (Date.now() < until) {
          const frame = await client.invokePluginRpc("paseo-screen", "screen.frame", { maxWidth, quality: 0.6 });
          replies += 1;
          kb = Math.round(frame.bytes / 1024);
        }
      }));
      console.log(`${maxWidth}px, ${lanes} in flight: ${(replies / 5).toFixed(1)} fps (${kb} KB/frame)`);
    }
  }
} finally {
  await client.close();
}
