// Calls paseo-screen plugin RPCs through the local daemon, like a remote client would.
// Usage: node scripts/rpc-probe.mjs <extracted Paseo app.asar dir> <method> [json input]
import { pathToFileURL } from "node:url";
import path from "node:path";
import { writeFileSync } from "node:fs";

const [asarDir, method, rawInput = "{}"] = process.argv.slice(2);
const cli = (file) => import(pathToFileURL(path.join(asarDir, "node_modules/@getpaseo/cli/dist/utils", file)).href);
const { connectToDaemon } = await cli("client.js");
const { selectDaemonTarget } = await cli("daemon-target.js");
const client = await connectToDaemon({ target: selectDaemonTarget({}) });
try {
  const started = Date.now();
  const output = await client.invokePluginRpc("paseo-screen", method, JSON.parse(rawInput));
  const elapsed = Date.now() - started;
  if (output?.jpeg) {
    const file = path.resolve("build", "probe-frame.jpg");
    writeFileSync(file, Buffer.from(output.jpeg, "base64"));
    output.jpeg = `<${output.jpeg.length} base64 chars, saved to ${file}>`;
  }
  console.log(JSON.stringify({ elapsedMs: elapsed, output }, null, 2));
} catch (error) {
  console.log("RPC error:", error?.message ?? error);
} finally {
  await client.close();
}
