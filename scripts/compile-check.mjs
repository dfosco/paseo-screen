// Compiles the plugin with Paseo's own plugin compiler to catch host-specific bundling errors.
// Usage: node scripts/compile-check.mjs <extracted Paseo app.asar dir>
import { pathToFileURL } from "node:url";
import path from "node:path";

const [asarDir] = process.argv.slice(2);
const plugin = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const { compilePlugin } = await import(pathToFileURL(path.join(asarDir, "node_modules/@getpaseo/server/dist/server/server/plugins/compiler.js")).href);
const result = await compilePlugin({ client: path.join(plugin, "index.client.tsx"), server: path.join(plugin, "index.server.ts") });
const size = (bundle) => (bundle ? JSON.stringify(bundle).length : 0);
console.log(`client bundle ~${Math.round(size(result.clientBundle) / 1024)} KB, server bundle ~${Math.round(size(result.serverBundle) / 1024)} KB`);
