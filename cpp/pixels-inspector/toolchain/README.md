# Inspector toolchain

The portable inspector pins Emscripten to the exact version in
`emscripten.version`. Do not replace it with `latest` in build or release
automation.

The protobuf schema and runtime are pinned by the repository's
`cpp/third-party/protobuf` submodule (currently v3.21.6). A reproducible build
uses two stages:

1. configure the native inspector build and build its `protoc` target;
2. configure the Emscripten build with
   `-DPIXELS_PROTOC_EXECUTABLE=<native-build>/third-party/protobuf/protoc`.

The inspector invokes the C++ generator in `lite` mode and links
`libprotobuf-lite`; this choice is local to the standalone target and does not
change the shared `pixels.proto` or the normal Core protobuf generation. The
WASM build cross-compiles that lite runtime from the same submodule and runs
the native host `protoc` to generate `pixels.pb.cc/.h`. It does not require a
JVM or any Pixels service.
