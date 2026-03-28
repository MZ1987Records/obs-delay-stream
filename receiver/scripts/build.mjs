import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const receiverDir = path.resolve(__dirname, '..');
const defaultOutDir = path.join(receiverDir, 'dist');
const outputDir = path.resolve(process.env.RECEIVER_BUILD_OUT_DIR || defaultOutDir);
const projectVersion = (process.env.RECEIVER_PROJECT_VERSION || '').trim();

const EXCLUDED_ROOT_ENTRIES = new Set([
  'dist',
  'node_modules',
  'scripts',
  'package.json',
  'package-lock.json',
  'npm-shrinkwrap.json',
]);

async function copyRecursive(srcPath, dstPath) {
  const stat = await fs.stat(srcPath);
  if (stat.isDirectory()) {
    await fs.mkdir(dstPath, { recursive: true });
    const entries = await fs.readdir(srcPath, { withFileTypes: true });
    for (const entry of entries) {
      await copyRecursive(path.join(srcPath, entry.name), path.join(dstPath, entry.name));
    }
    return;
  }

  await fs.mkdir(path.dirname(dstPath), { recursive: true });
  await fs.copyFile(srcPath, dstPath);
}

async function main() {
  await fs.rm(outputDir, { recursive: true, force: true });
  await fs.mkdir(outputDir, { recursive: true });

  const rootEntries = await fs.readdir(receiverDir, { withFileTypes: true });
  for (const entry of rootEntries) {
    if (EXCLUDED_ROOT_ENTRIES.has(entry.name)) {
      continue;
    }

    const srcPath = path.join(receiverDir, entry.name);
    const dstPath = path.join(outputDir, entry.name);
    await copyRecursive(srcPath, dstPath);
  }

  const indexPath = path.join(outputDir, 'index.html');
  let indexHtml = await fs.readFile(indexPath, 'utf8');
  if (projectVersion) {
    indexHtml = indexHtml.replaceAll('@PROJECT_VERSION@', projectVersion);
  }
  await fs.writeFile(indexPath, indexHtml, 'utf8');

  process.stdout.write(`[receiver-build] output: ${outputDir}\n`);
}

main().catch((error) => {
  process.stderr.write(`[receiver-build] failed: ${error instanceof Error ? error.stack : String(error)}\n`);
  process.exit(1);
});
