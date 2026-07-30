# Pixels portable inspector

`pixels-inspector` is the smallest portable dependency closure beneath the
native Pixels Reader. It contains:

- checked immutable byte spans and integer/range operations;
- FileTail and RowGroupFooter protobuf parsing and validation;
- pure per-encoding decoders for packed BOOLEAN, fixed-width scalars,
  temporal values, exact short/long decimals, variable-width strings,
  binary values, VECTOR, and recursively nested ARRAY/MAP/STRUCT values;
- a host-driven byte-range state machine;
- a versioned C ABI compiled from the same sources for native and WebAssembly.

It deliberately does not link physical storage, Scheduler, BufferPool,
liburing, DuckDB, global configuration, platform SIMD, or VS Code. The host
owns asynchronous I/O and supplies only the exact range requested by the
session.

## Native conformance

Initialize only the protobuf nested submodule:

```sh
git submodule update --init cpp/third-party/protobuf
cmake -S cpp/pixels-inspector -B <native-build> \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPIXELS_INSPECTOR_ENABLE_SANITIZERS=ON
cmake --build <native-build> --target pixels-inspector-tests
ctest --test-dir <native-build> --output-on-failure
<native-build>/pixels-inspector-tests --write-corpus <corpus-dir>
```

This builds the host `protoc` and protobuf-lite runtime from the repository's
pinned protobuf v3.21.6 source. It does not use Java or start a service.

## WASM conformance

Install the exact Emscripten version recorded in
`toolchain/emscripten.version`, then reuse the native host `protoc`:

```sh
emcmake cmake -S cpp/pixels-inspector -B <wasm-build> \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIXELS_INSPECTOR_BUILD_TESTS=OFF \
  -DPIXELS_PROTOC_EXECUTABLE=<native-build>/third-party/protobuf/protoc
cmake --build <wasm-build> --target pixels-inspector-wasm
node cpp/pixels-inspector/tools/run_wasm_worker_conformance.cjs \
  <wasm-build>/pixels-inspector-wasm.cjs \
  cpp/tests/data/example.pxl \
  <corpus-dir>
```

The conformance runner validates the ABI, exact metadata, row-group layout,
generic pages, `rows-v1`, `filter-v1`, cancellation, mismatched ranges, all 20
logical kinds, the declared encoding compatibility matrix, DECIMAL precision
boundaries, TIMESTAMP precision and writer-timezone provenance, bounded linear
memory, and the Emscripten import allowlist.

The generic page operation supports bounded pages across pixels, padded and
compacted null layouts, integer RLE, dictionary and plain variable-width
strings, binary values, VECTOR, and the portable nested-column contract in
`NESTED_LAYOUT.md`. Unsupported encodings return an explicit error instead of
falling back to guessed bytes.

`rows-v1` accepts an ordered unique projection of root columns and at most 500
rows. `filter-v1` accepts one typed root-scalar predicate, returns at most 500
matching rows (100 when the ABI limit argument is zero), and uses a Core-owned
continuation. Missing or ambiguous statistics always fall back to scanning.

Pixels V1 declares `NONE`, `ZLIB`, `SNAPPY`, `LZO`, `LZ4`, and `ZSTD` in
PostScript, but the format marks the compression fields as currently unused.
ABI v3 therefore preserves all six recognized metadata values and advertises
compression payload capability as `inactive`; it does not route chunk bytes
through a guessed decompressor.

## Range protocol

1. create a session with the immutable file size;
2. begin metadata, row-group layout, page, row projection, or filter;
3. call `next_range`;
4. asynchronously read that exact range in the host;
5. call `supply_range`;
6. repeat until the status is `RESULT_READY`;
7. size and copy the result into caller-owned memory;
8. destroy or cancel the session.

Only one range may be pending. Partial, oversized, duplicate, or out-of-order
supplies make the session terminal. No C++ object, exception, STL type, or
borrowed result pointer crosses `pixels_inspector.h`.
