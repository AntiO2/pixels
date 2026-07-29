#!/usr/bin/env node

"use strict";

const path = require("node:path");
const { Worker } = require("node:worker_threads");

const harnessPath = path.join(__dirname, "run_wasm_conformance.cjs");
const modulePath = path.resolve(process.argv[2] ?? "");
const fixturePath = path.resolve(process.argv[3] ?? "");

function runWorker() {
  return new Promise((resolve, reject) => {
    const worker = new Worker(harnessPath, {
      argv: [modulePath, fixturePath],
      stdout: true,
      stderr: true,
    });

    let stdout = "";
    let stderr = "";
    worker.stdout.setEncoding("utf8");
    worker.stderr.setEncoding("utf8");
    worker.stdout.on("data", (chunk) => {
      stdout += chunk;
    });
    worker.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    worker.once("error", reject);
    worker.once("exit", (exitCode) => {
      if (exitCode !== 0) {
        reject(new Error(`worker exited with ${exitCode}\n${stderr}`));
        return;
      }
      try {
        resolve(JSON.parse(stdout.trim()));
      } catch (error) {
        reject(new Error(`${error.stack}\n${stdout}${stderr}`));
      }
    });
  });
}

async function main() {
  const first = await runWorker();
  const restarted = await runWorker();
  if (JSON.stringify(first.metadata) !== JSON.stringify(restarted.metadata)
      || JSON.stringify(first.page) !== JSON.stringify(restarted.page)) {
    throw new Error("recreated worker produced different inspection results");
  }
  restarted.nodeWorker = true;
  restarted.workerRestart = true;
  process.stdout.write(`${JSON.stringify(restarted)}\n`);
}

main().catch((error) => {
  process.stderr.write(
    `pixels-inspector worker conformance: FAIL: ${error.stack}\n`,
  );
  process.exitCode = 1;
});
