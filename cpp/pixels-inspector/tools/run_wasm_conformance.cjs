#!/usr/bin/env node

"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { performance } = require("node:perf_hooks");

const EXPECTED_METADATA =
  '{"abi":3,"version":1,"magic":"PIXELS","rows":10,' +
  '"pixelStride":10000,"schemaCount":4,"rowGroupCount":1,' +
  '"firstColumn":{"name":"id","kind":4},' +
  '"postscript":{"contentLength":"352","compression":0,' +
  '"compressionBlockSize":1,' +
  '"writerTimezone":"Central European Standard Time",' +
  '"partitioned":false,"columnChunkAlignment":32,' +
  '"hasHiddenColumn":false},"schema":[' +
  '{"id":0,"name":"id","kind":4,"subtypes":[]},' +
  '{"id":1,"name":"name","kind":16,"subtypes":[],"maximumLength":25},' +
  '{"id":2,"name":"birthday","kind":15,"subtypes":[]},' +
  '{"id":3,"name":"score","kind":14,"subtypes":[],' +
  '"precision":15,"scale":2}],"fileStatistics":[' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"integer":{"minimum":"0","maximum":"9","sum":"45"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"string":{"minimum":"Alice","maximum":"Tom","sum":"47"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"date":{"minimum":"-25202","maximum":"14389"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"integer":{"minimum":"740","maximum":"10001","sum":"66057"}}],' +
  '"rowGroups":[{"index":0,"footerOffset":"352","footerLength":154,' +
  '"dataLength":352,"rows":10}],"rowGroupStatistics":[[' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"integer":{"minimum":"0","maximum":"9","sum":"45"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"string":{"minimum":"Alice","maximum":"Tom","sum":"47"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"date":{"minimum":"-25202","maximum":"14389"}},' +
  '{"numberOfValues":"10","containsNull":false,' +
  '"integer":{"minimum":"740","maximum":"10001","sum":"66057"}}]]}';

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
  const corpusPath = process.argv[4] === undefined ||
    process.argv[4] === ""
    ? ""
    : path.resolve(process.argv[4]);
  requireCondition(fs.existsSync(modulePath), "WASM JavaScript module is missing");
  requireCondition(fs.existsSync(fixturePath), "canonical fixture is missing");
  requireCondition(
    corpusPath === "" || fs.existsSync(path.join(corpusPath, "manifest.json")),
    "conformance corpus manifest is missing",
  );

  const createPixelsInspector = require(modulePath);
  const startedAt = performance.now();
  const module = await createPixelsInspector({
    locateFile(fileName) {
      return path.join(path.dirname(modulePath), fileName);
    },
  });
  const initializedAt = performance.now();
  let file = fs.readFileSync(fixturePath);
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
    requireCondition(module._pixels_inspector_abi_version() === 3,
      "unexpected inspector ABI");
    requireCondition(
      module._pixels_inspector_capabilities_size(scratch) === 0,
      "capability size is unavailable",
    );
    const capabilitySize = Number(
      new DataView(module.HEAPU8.buffer, scratch, 8)
        .getBigUint64(0, true),
    );
    const capabilityPointer = module._malloc(capabilitySize);
    let capabilities;
    try {
      requireCondition(
        module._pixels_inspector_copy_capabilities(
          capabilityPointer,
          BigInt(capabilitySize),
        ) === 0,
        "unable to copy capabilities",
      );
      capabilities = JSON.parse(
        new TextDecoder().decode(
          module.HEAPU8.slice(
            capabilityPointer,
            capabilityPointer + capabilitySize,
          ),
        ),
      );
    } finally {
      module._free(capabilityPointer);
    }
    requireCondition(
      capabilities.abi === 3 &&
        capabilities.page === "generic-v1" &&
        capabilities.rows === "rows-v1" &&
        capabilities.filter === "filter-v1" &&
        capabilities.maxRows === 500 &&
        capabilities.defaultRows === 100 &&
        capabilities.compression?.payload === "inactive" &&
        JSON.stringify(capabilities.compression?.metadata) ===
          JSON.stringify(["NONE", "ZLIB", "SNAPPY", "LZO", "LZ4", "ZSTD"]) &&
        capabilities.types.length === 20 &&
        capabilities.types.every(
          (type, index) => type.kind === index && typeof type.name === "string",
        ),
      "Core capability inventory is incomplete",
    );
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

    requireCondition(
      module._pixels_inspector_begin_row_group(handle, 0) === 1,
      "row-group layout did not request its footer",
    );
    requireCondition(supplyRange(nextRange()) === 2,
      "row-group footer did not produce layout");
    const rowGroupLayout = JSON.parse(readResult());
    requireCondition(
      rowGroupLayout.rowGroup === 0 &&
        rowGroupLayout.columns.length === 4 &&
        rowGroupLayout.columns[0].chunk.offset === "0" &&
        rowGroupLayout.columns[1].chunk.length === 95 &&
        rowGroupLayout.columns[3].chunk.pixels[0]
          .statistics.integer.sum === "66057",
      "WASM row-group layout differs from the native golden",
    );

    const pageStartedAt = performance.now();
    requireCondition(
      module._pixels_inspector_begin_page(
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

    const projectionPointer = module._malloc(8);
    const literalPointer = module._malloc(1);
    try {
      const projectionView =
        new DataView(module.HEAPU8.buffer, projectionPointer, 8);
      projectionView.setUint32(0, 0, true);
      projectionView.setUint32(4, 1, true);
      requireCondition(
        module._pixels_inspector_begin_rows(
          handle, 0, projectionPointer, 2, 2n, 3,
        ) === 1,
        "rows-v1 did not start",
      );
      let status = 1;
      let suppliedRanges = 0;
      while (status === 1) {
        status = supplyRange(nextRange());
        suppliedRanges += 1;
        requireCondition(suppliedRanges < 64,
          "rows-v1 did not converge");
      }
      requireCondition(status === 2, "rows-v1 did not produce a result");
      const rows = JSON.parse(readResult());
      requireCondition(
        rows.operation === "rows-v1" &&
          rows.columns.map(({ id }) => id).join(",") === "0,1" &&
          rows.rows.length === 3 &&
          rows.rows[0].localRow === "2" &&
          rows.rows.every((row) => row.values.length === 2),
        "WASM rows-v1 differs from the native contract",
      );

      module.HEAPU8[literalPointer] = "8".charCodeAt(0);
      requireCondition(
        module._pixels_inspector_begin_filter(
          handle, 0, 5, literalPointer, 1,
          projectionPointer, 2, 0, 0, 100,
        ) === 1,
        "filter-v1 did not start",
      );
      status = 1;
      suppliedRanges = 0;
      while (status === 1) {
        status = supplyRange(nextRange());
        suppliedRanges += 1;
        requireCondition(suppliedRanges < 128,
          "filter-v1 did not converge");
      }
      requireCondition(status === 2,
        "filter-v1 did not produce a result");
      const filter = JSON.parse(readResult());
      requireCondition(
        filter.operation === "filter-v1" &&
          filter.rows.length === 2 &&
          filter.rows[0].localRow === "8" &&
          filter.rows[1].localRow === "9" &&
          filter.completed === true &&
          filter.truncated === false &&
          filter.cursor === null,
        "WASM filter-v1 differs from the native contract",
      );
    } finally {
      module._free(literalPointer);
      module._free(projectionPointer);
    }

    let corpusCases = 0;
    if (corpusPath !== "") {
      const corpus = JSON.parse(
        fs.readFileSync(path.join(corpusPath, "manifest.json"), "utf8"),
      );
      requireCondition(corpus.abi === 3 && corpus.cases.length === 20 &&
        Array.isArray(corpus.compatibility) && corpus.compatibility.length > 0,
        "conformance corpus inventory is incomplete");
      const expectedNames = capabilities.types.map((type) => type.name);
      requireCondition(
        JSON.stringify(corpus.cases.map((test) => test.name).sort()) ===
          JSON.stringify([...expectedNames].sort()),
        "corpus kinds differ from Core capabilities",
      );
      for (const test of [...corpus.cases, ...corpus.compatibility]) {
        requireCondition(module._pixels_inspector_destroy(handle) === 0,
          `unable to reset session for ${test.name}`);
        handle = 0;
        file = fs.readFileSync(path.join(corpusPath, test.file));
        requireCondition(
          module._pixels_inspector_create(
            BigInt(file.length),
            handlePointer,
          ) === 0,
          `unable to create ${test.name} session`,
        );
        handle = new DataView(
          module.HEAPU8.buffer,
          handlePointer,
          4,
        ).getUint32(0, true);
        requireCondition(
          module._pixels_inspector_begin_metadata(handle) === 1,
          `${test.name} metadata did not start`,
        );
        requireCondition(supplyRange(nextRange()) === 1,
          `${test.name} tail pointer was rejected`);
        requireCondition(supplyRange(nextRange()) === 2,
          `${test.name} FileTail was rejected`);
        requireCondition(
          module._pixels_inspector_begin_page(
            handle,
            0,
            test.column,
            BigInt(test.offset),
            test.count,
          ) === 1,
          `${test.name} page did not start`,
        );
        let status = 1;
        let suppliedRanges = 0;
        while (status === 1) {
          status = supplyRange(nextRange());
          suppliedRanges += 1;
          requireCondition(suppliedRanges < 64,
            `${test.name} page did not converge`);
        }
        requireCondition(status === 2,
          `${test.name} page ended with status ${status}`);
        requireCondition(
          JSON.stringify(JSON.parse(readResult())) ===
            JSON.stringify(test.expected),
          `${test.name} WASM page differs from the native golden`,
        );
        corpusCases += 1;
      }
      file = fs.readFileSync(fixturePath);
    }

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
      abi: 3,
      capabilities,
      corpusCases,
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
