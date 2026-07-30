# Pixels portable inspector

`pixels-inspector` is the smallest portable dependency closure beneath the
native Pixels Reader. It contains:

- checked immutable byte spans and integer/range operations;
- FileTail and RowGroupFooter protobuf parsing and validation;
- pure per-encoding decoders (the current `NONE` scalar slice includes packed
  BOOLEAN, BYTE, 32/64-bit integers, FLOAT/DOUBLE, temporal values, and
  short/long-decimal storage words);
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
  cpp/tests/data/example.pxl
```

The conformance runner validates the ABI, exact metadata/page output,
cancellation, mismatched ranges, legacy LONG plus generic DATE output, bounded
linear memory, and the Emscripten import allowlist.

The generic page operation currently promotes `NONE` scalar pages within one
pixel, including padded and compacted null layouts with exact bitmap/value
ranges. Multi-pixel pages, RLE, variable-width, vector, and nested values remain
explicit follow-up work; the API returns an unsupported status instead of
guessing their layout.

## Range protocol

1. create a session with the immutable file size;
2. begin metadata or a bounded page request;
3. call `next_range`;
4. asynchronously read that exact range in the host;
5. call `supply_range`;
6. repeat until the status is `RESULT_READY`;
7. size and copy the result into caller-owned memory;
8. destroy or cancel the session.

Only one range may be pending. Partial, oversized, duplicate, or out-of-order
supplies make the session terminal. No C++ object, exception, STL type, or
borrowed result pointer crosses `pixels_inspector.h`.
