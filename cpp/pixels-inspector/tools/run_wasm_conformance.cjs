#!/usr/bin/env node

"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { performance } = require("node:perf_hooks");

const EXPECTED_METADATA =
  '{"abi":1,"version":1,"magic":"PIXELS","rows":10,' +
  '"pixelStride":10000,"schemaCount":4,"rowGroupCount":1,' +
  '"firstColumn":{"name":"id","kind":4}}';

const EXPECTED_PAGE =
  '{"rowGroup":0,"column":0,"offset":0,"count":10,' +
  '"values":["0","1","2","3","4","5","6","7","8","9"]}';

const EXPECTED_DATE_PAGE =
  '{"rowGroup":0,"column":2,"offset":1,"count":3,' +
  '"values":["10227","10207","11208"]}';

function requireCondition(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

async function main() {
  const modulePath = path.resolve(process.argv[2] ?? "");
  const fixturePath = path.resolve(process.argv[3] ?? "");
  requireCondition(fs.existsSync(modulePath), "WASM JavaScript module is missing");
  requireCondition(fs.existsSync(fixturePath), "canonical fixture is missing");

  const createPixelsInspector = require(modulePath);
  const startedAt = performance.now();
  const module = await createPixelsInspector({
    locateFile(fileName) {
      return path.join(path.dirname(modulePath), fileName);
    },
  });
  const initializedAt = performance.now();
  const file = fs.readFileSync(fixturePath);
  requireCondition(file.length === 790, "canonical fixture length changed");
  const wasmPath = modulePath.replace(/\.(?:c?js)$/, ".wasm");
  const wasmBytes = fs.readFileSync(wasmPath);
  const rawWasmImports = WebAssembly.Module.imports(
    new WebAssembly.Module(wasmBytes),
  ).map(({ module: importModule, name }) => `${importModule}.${name}`);
  const moduleSource = fs.readFileSync(modulePath, "utf8");
  const importObjectMatch = moduleSource.match(/var wasmImports=\{([^}]+)\}/);
  requireCondition(importObjectMatch !== null,
    "unable to inspect Emscripten import mapping");
  const wasmImports = importObjectMatch[1]
    .split(",")
    .map((entry) => entry.slice(entry.indexOf(":") + 1));
  const allowedEmscriptenImports = new Set([
    "___cxa_throw",
    "__abort_js",
    "_emscripten_resize_heap",
    "_environ_get",
    "_environ_sizes_get",
    // With FILESYSTEM=0 these are non-filesystem libc compatibility shims:
    // close/seek return an error and write only forwards stdout/stderr.
    "_fd_close",
    "_fd_seek",
    "_fd_write",
  ]);
  requireCondition(
    wasmImports.every((name) => allowedEmscriptenImports.has(name)),
    `unexpected Emscripten import: ${wasmImports.find(
      (name) => !allowedEmscriptenImports.has(name),
    )}`,
  );
  const forbiddenImports =
    /(wasi|filesystem|(^|\\.)fd_|path_|pthread|thread|uring|duckdb|avx)/i;
  requireCondition(
    rawWasmImports.every((name) => !forbiddenImports.test(name)),
    `forbidden raw WASM import: ${rawWasmImports.find(
      (name) => forbiddenImports.test(name),
    )}`,
  );

  const scratch = module._malloc(16);
  const handlePointer = module._malloc(4);
  let handle = 0;
  let rangeCopies = 0;
  try {
    requireCondition(module._pixels_inspector_abi_version() === 1,
      "unexpected inspector ABI");
    requireCondition(
      module._pixels_inspector_create(BigInt(file.length), handlePointer) === 0,
      "unable to create session",
    );
    handle = new DataView(
      module.HEAPU8.buffer,
      handlePointer,
      4,
    ).getUint32(0, true);

    function nextRange() {
      const status = module._pixels_inspector_next_range(
        handle,
        scratch,
        scratch + 8,
      );
      requireCondition(status === 1, `next_range returned ${status}`);
      const view = new DataView(module.HEAPU8.buffer, scratch, 16);
      return {
        offset: Number(view.getBigUint64(0, true)),
        length: Number(view.getBigUint64(8, true)),
      };
    }

    function supplyRange(range) {
      requireCondition(
        range.offset >= 0 &&
          range.length >= 0 &&
          range.offset + range.length <= file.length,
        "module requested an invalid fixture range",
      );
      const bytesPointer = module._malloc(range.length);
      try {
        module.HEAPU8.set(
          file.subarray(range.offset, range.offset + range.length),
          bytesPointer,
        );
        rangeCopies += 1;
        return module._pixels_inspector_supply_range(
          handle,
          BigInt(range.offset),
          BigInt(range.length),
          bytesPointer,
        );
      } finally {
        module._free(bytesPointer);
      }
    }

    function readResult() {
      requireCondition(
        module._pixels_inspector_result_size(handle, scratch) === 0,
        "result size is unavailable",
      );
      const size = Number(
        new DataView(module.HEAPU8.buffer, scratch, 8)
          .getBigUint64(0, true),
      );
      const resultPointer = module._malloc(size);
      try {
        requireCondition(
          module._pixels_inspector_copy_result(
            handle,
            resultPointer,
            BigInt(size),
          ) === 0,
          "unable to copy result",
        );
        return new TextDecoder().decode(
          module.HEAPU8.slice(resultPointer, resultPointer + size),
        );
      } finally {
        module._free(resultPointer);
      }
    }

    requireCondition(
      module._pixels_inspector_begin_metadata(handle) === 1,
      "metadata did not request the tail pointer",
    );
    requireCondition(supplyRange(nextRange()) === 1,
      "tail pointer did not request FileTail");
    requireCondition(supplyRange(nextRange()) === 2,
      "FileTail did not produce metadata");
    requireCondition(readResult() === EXPECTED_METADATA,
      "WASM metadata differs from the native golden");

    const pageStartedAt = performance.now();
    requireCondition(
      module._pixels_inspector_begin_plain_long_page(
        handle,
        0,
        0,
        0n,
        10,
      ) === 1,
      "page did not request its footer",
    );
    requireCondition(supplyRange(nextRange()) === 1,
      "footer did not request a column chunk");
    requireCondition(supplyRange(nextRange()) === 2,
      "column chunk did not produce a page");
    requireCondition(readResult() === EXPECTED_PAGE,
      "WASM page differs from the native golden");
    const pageCompletedAt = performance.now();

    requireCondition(module._pixels_inspector_destroy(handle) === 0,
      "unable to destroy LONG page session");
    handle = 0;
    requireCondition(
      module._pixels_inspector_create(BigInt(file.length), handlePointer) === 0,
      "unable to create generic page session",
    );
    handle = new DataView(
      module.HEAPU8.buffer,
      handlePointer,
      4,
    ).getUint32(0, true);
    requireCondition(module._pixels_inspector_begin_metadata(handle) === 1,
      "generic page metadata did not start");
    requireCondition(supplyRange(nextRange()) === 1,
      "generic page tail pointer did not request FileTail");
    requireCondition(supplyRange(nextRange()) === 2,
      "generic page FileTail did not produce metadata");
    requireCondition(
      module._pixels_inspector_begin_page(handle, 0, 2, 1n, 3) === 1,
      "generic DATE page did not request its footer",
    );
    requireCondition(supplyRange(nextRange()) === 1,
      "generic DATE footer did not request values");
    requireCondition(supplyRange(nextRange()) === 2,
      "generic DATE values did not produce a page");
    requireCondition(readResult() === EXPECTED_DATE_PAGE,
      "WASM generic DATE page differs from the native golden");

    requireCondition(
      module._pixels_inspector_create(BigInt(file.length), scratch) === 0,
      "unable to create cancellation probe",
    );
    const cancellationHandle = new DataView(
      module.HEAPU8.buffer,
      scratch,
      4,
    ).getUint32(0, true);
    requireCondition(
      module._pixels_inspector_begin_metadata(cancellationHandle) === 1,
      "cancellation probe did not start",
    );
    requireCondition(
      module._pixels_inspector_cancel(cancellationHandle) === 108,
      "WASM cancellation did not reach its terminal status",
    );
    requireCondition(
      module._pixels_inspector_destroy(cancellationHandle) === 0,
      "unable to destroy cancellation probe",
    );

    requireCondition(
      module._pixels_inspector_create(BigInt(file.length), scratch) === 0,
      "unable to create invalid-range probe",
    );
    const invalidRangeHandle = new DataView(
      module.HEAPU8.buffer,
      scratch,
      4,
    ).getUint32(0, true);
    requireCondition(
      module._pixels_inspector_begin_metadata(invalidRangeHandle) === 1,
      "invalid-range probe did not start",
    );
    requireCondition(
      module._pixels_inspector_next_range(
        invalidRangeHandle,
        scratch,
        scratch + 8,
      ) === 1,
      "invalid-range probe did not expose a range",
    );
    const invalidRangeView = new DataView(module.HEAPU8.buffer, scratch, 16);
    const expectedOffset = Number(invalidRangeView.getBigUint64(0, true));
    const expectedLength = Number(invalidRangeView.getBigUint64(8, true));
    const invalidBytesPointer = module._malloc(expectedLength);
    try {
      module.HEAPU8.set(
        file.subarray(expectedOffset, expectedOffset + expectedLength),
        invalidBytesPointer,
      );
      requireCondition(
        module._pixels_inspector_supply_range(
          invalidRangeHandle,
          BigInt(expectedOffset - 1),
          BigInt(expectedLength),
          invalidBytesPointer,
        ) === 100,
        "WASM mismatched range was not rejected",
      );
    } finally {
      module._free(invalidBytesPointer);
    }
    requireCondition(
      module._pixels_inspector_destroy(invalidRangeHandle) === 0,
      "unable to destroy invalid-range probe",
    );

    const metrics = {
      abi: 1,
      wasmBytes: fs.statSync(wasmPath).size,
      initializationMs: Number((initializedAt - startedAt).toFixed(3)),
      pageMs: Number((pageCompletedAt - pageStartedAt).toFixed(3)),
      linearMemoryBytes: module.HEAPU8.buffer.byteLength,
      rangeCopies,
      failureChecks: ["cancel", "mismatched-range"],
      imports: wasmImports,
      rawImports: rawWasmImports,
      metadata: EXPECTED_METADATA,
      page: EXPECTED_PAGE,
    };
    process.stdout.write(`${JSON.stringify(metrics)}\n`);
  } finally {
    if (handle !== 0) {
      module._pixels_inspector_destroy(handle);
    }
    module._free(handlePointer);
    module._free(scratch);
  }
}

main().catch((error) => {
  process.stderr.write(`pixels-inspector WASM conformance: FAIL: ${error.stack}\n`);
  process.exitCode = 1;
});
