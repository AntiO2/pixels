cmake_minimum_required(VERSION 3.19)

project(pixels-inspector LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PIXELS_INSPECTOR_BUILD_TESTS "Build native inspector tests" ON)
option(PIXELS_INSPECTOR_ENABLE_SANITIZERS
       "Enable address and undefined behavior sanitizers" OFF)
set(PIXELS_PROTOC_EXECUTABLE "" CACHE FILEPATH
    "Host protoc used when cross-compiling")

get_filename_component(PIXELS_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(PIXELS_SOURCE_ROOT "${PIXELS_CPP_ROOT}/.." ABSOLUTE)
set(PIXELS_PROTOBUF_ROOT "${PIXELS_CPP_ROOT}/third-party/protobuf")
set(PIXELS_PROTO_FILE "${PIXELS_SOURCE_ROOT}/proto/pixels.proto")
set(PIXELS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(EMSCRIPTEN)
    file(READ
        "${CMAKE_CURRENT_SOURCE_DIR}/toolchain/emscripten.version"
        PIXELS_EMSCRIPTEN_VERSION)
    string(STRIP "${PIXELS_EMSCRIPTEN_VERSION}"
           PIXELS_EMSCRIPTEN_VERSION)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" --version
        RESULT_VARIABLE PIXELS_EMSCRIPTEN_VERSION_RESULT
        OUTPUT_VARIABLE PIXELS_EMSCRIPTEN_VERSION_OUTPUT
        ERROR_VARIABLE PIXELS_EMSCRIPTEN_VERSION_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(FIND "${PIXELS_EMSCRIPTEN_VERSION_OUTPUT}"
           ") ${PIXELS_EMSCRIPTEN_VERSION} ("
           PIXELS_EMSCRIPTEN_VERSION_INDEX)
    if(NOT PIXELS_EMSCRIPTEN_VERSION_RESULT EQUAL 0
       OR PIXELS_EMSCRIPTEN_VERSION_INDEX EQUAL -1)
        message(FATAL_ERROR
            "pixels-inspector requires Emscripten "
            "${PIXELS_EMSCRIPTEN_VERSION}; compiler reported: "
            "${PIXELS_EMSCRIPTEN_VERSION_OUTPUT}"
            "${PIXELS_EMSCRIPTEN_VERSION_ERROR}")
    endif()
endif()

add_custom_target(pixels-inspector-check-dependencies ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DPIXELS_CPP_ROOT=${PIXELS_CPP_ROOT}"
            "-DPIXELS_INSPECTOR_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_portable_dependencies.cmake"
    COMMENT "Checking portable inspector dependency boundary"
    VERBATIM)

if(NOT EXISTS "${PIXELS_PROTOBUF_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
            "protobuf submodule is missing; initialize cpp/third-party/protobuf")
endif()

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
if(CMAKE_CROSSCOMPILING OR PIXELS_PROTOC_EXECUTABLE)
    set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
else()
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${PIXELS_PROTOBUF_ROOT}"
                 "${CMAKE_CURRENT_BINARY_DIR}/third-party/protobuf"
                 EXCLUDE_FROM_ALL)

if(CMAKE_CROSSCOMPILING OR PIXELS_PROTOC_EXECUTABLE)
    if(NOT PIXELS_PROTOC_EXECUTABLE
       OR NOT EXISTS "${PIXELS_PROTOC_EXECUTABLE}")
        message(FATAL_ERROR
                "This build requires -DPIXELS_PROTOC_EXECUTABLE=<host protoc>")
    endif()
    set(PIXELS_PROTOC_COMMAND "${PIXELS_PROTOC_EXECUTABLE}")
    set(PIXELS_PROTOC_DEPENDS)
else()
    set(PIXELS_PROTOC_COMMAND "$<TARGET_FILE:protoc>")
    set(PIXELS_PROTOC_DEPENDS protoc)
endif()

file(MAKE_DIRECTORY "${PIXELS_GENERATED_DIR}")
add_custom_command(
    OUTPUT
        "${PIXELS_GENERATED_DIR}/pixels.pb.cc"
        "${PIXELS_GENERATED_DIR}/pixels.pb.h"
    COMMAND "${PIXELS_PROTOC_COMMAND}"
            "--proto_path=${PIXELS_SOURCE_ROOT}/proto"
            "--cpp_out=lite:${PIXELS_GENERATED_DIR}"
            "${PIXELS_PROTO_FILE}"
    DEPENDS "${PIXELS_PROTO_FILE}" ${PIXELS_PROTOC_DEPENDS}
    COMMENT "Generating Pixels protobuf C++ sources"
    VERBATIM)

set(PIXELS_COMPAT_PROTO_HEADER
    "${PIXELS_GENERATED_DIR}/pixels-common/pixels.pb.h")
add_custom_command(
    OUTPUT "${PIXELS_COMPAT_PROTO_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${PIXELS_GENERATED_DIR}/pixels-common"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PIXELS_GENERATED_DIR}/pixels.pb.h"
            "${PIXELS_COMPAT_PROTO_HEADER}"
    DEPENDS "${PIXELS_GENERATED_DIR}/pixels.pb.h"
    COMMENT "Creating native Core protobuf include compatibility path"
    VERBATIM)
add_custom_target(pixels-inspector-compat-proto
    DEPENDS "${PIXELS_COMPAT_PROTO_HEADER}")

add_library(pixels-inspector-proto STATIC
    "${PIXELS_GENERATED_DIR}/pixels.pb.cc")
target_include_directories(pixels-inspector-proto PUBLIC
    "${PIXELS_GENERATED_DIR}")
target_include_directories(pixels-inspector-proto SYSTEM PUBLIC
    "${PIXELS_PROTOBUF_ROOT}/src")
target_link_libraries(pixels-inspector-proto PUBLIC libprotobuf-lite)

add_library(pixels-format STATIC
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/PixelsFormatReader.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/PlainPixelPlanner.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/PlainScalarDecoder.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/PlainLongDecoder.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/RunLengthByteDecoder.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/RunLengthIntDecoder.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/SchemaValidator.cpp"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/VariableLengthDecoder.cpp")
target_include_directories(pixels-format PUBLIC
    "${PIXELS_CPP_ROOT}/pixels-core/include")
target_link_libraries(pixels-format PUBLIC pixels-inspector-proto)
add_dependencies(pixels-format pixels-inspector-check-dependencies)

add_library(pixels-inspector STATIC
    lib/ScanComparator.cpp
    lib/ScanCursor.cpp
    lib/ScanExpression.cpp
    lib/ScanPlan.cpp
    lib/ScanRuntime.cpp
    lib/InspectionSession.cpp
    lib/pixels_inspector.cpp)
target_include_directories(pixels-inspector PUBLIC
    include)
target_link_libraries(pixels-inspector PUBLIC pixels-format)

foreach(target pixels-format pixels-inspector)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

if(PIXELS_INSPECTOR_ENABLE_SANITIZERS)
    if(CMAKE_CROSSCOMPILING OR MSVC)
        message(FATAL_ERROR
                "Sanitizers are supported only by native non-MSVC builds")
    endif()
    foreach(target pixels-format pixels-inspector)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()

if(PIXELS_INSPECTOR_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_library(pixels-reader-adapter-compile OBJECT
        "${PIXELS_CPP_ROOT}/pixels-core/lib/PixelsReaderBuilder.cpp"
        "${PIXELS_CPP_ROOT}/pixels-core/lib/reader/LongColumnReader.cpp"
        "${PIXELS_CPP_ROOT}/pixels-core/lib/reader/PixelsRecordReaderImpl.cpp"
        "${PIXELS_CPP_ROOT}/pixels-core/lib/vector/LongColumnVector.cpp")
    target_include_directories(pixels-reader-adapter-compile PRIVATE
        "${PIXELS_CPP_ROOT}/pixels-core/include"
        "${PIXELS_CPP_ROOT}/pixels-common/include"
        "${PIXELS_GENERATED_DIR}")
    target_link_libraries(pixels-reader-adapter-compile PRIVATE pixels-format)
    add_dependencies(pixels-reader-adapter-compile
        pixels-inspector-compat-proto)

    add_executable(pixels-inspector-tests tests/InspectorTests.cpp)
    target_link_libraries(pixels-inspector-tests PRIVATE pixels-inspector)
    add_dependencies(pixels-inspector-tests pixels-reader-adapter-compile)
    target_compile_definitions(pixels-inspector-tests PRIVATE
        PIXELS_TEST_FIXTURE="${PIXELS_CPP_ROOT}/tests/data/example.pxl")
    if(PIXELS_INSPECTOR_ENABLE_SANITIZERS)
        target_compile_options(pixels-inspector-tests PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pixels-inspector-tests PRIVATE
            -fsanitize=address,undefined)
    endif()
    add_test(NAME pixels-inspector-tests COMMAND pixels-inspector-tests)

    add_executable(
        pixels-inspector-scan-plan-tests tests/ScanPlanTests.cpp)
    target_link_libraries(
        pixels-inspector-scan-plan-tests PRIVATE pixels-inspector)
    if(PIXELS_INSPECTOR_ENABLE_SANITIZERS)
        target_compile_options(pixels-inspector-scan-plan-tests PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pixels-inspector-scan-plan-tests PRIVATE
            -fsanitize=address,undefined)
    endif()
    add_test(
        NAME pixels-inspector-scan-plan-tests
        COMMAND pixels-inspector-scan-plan-tests)

    add_executable(
        pixels-inspector-rle-tests tests/RunLengthDecoderTests.cpp)
    target_link_libraries(
        pixels-inspector-rle-tests PRIVATE pixels-format)
    if(PIXELS_INSPECTOR_ENABLE_SANITIZERS)
        target_compile_options(pixels-inspector-rle-tests PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pixels-inspector-rle-tests PRIVATE
            -fsanitize=address,undefined)
    endif()
    add_test(
        NAME pixels-inspector-rle-tests
        COMMAND pixels-inspector-rle-tests)

    add_executable(
        pixels-inspector-variable-tests
        tests/VariableLengthDecoderTests.cpp)
    target_link_libraries(
        pixels-inspector-variable-tests PRIVATE pixels-format)
    if(PIXELS_INSPECTOR_ENABLE_SANITIZERS)
        target_compile_options(pixels-inspector-variable-tests PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pixels-inspector-variable-tests PRIVATE
            -fsanitize=address,undefined)
    endif()
    add_test(
        NAME pixels-inspector-variable-tests
        COMMAND pixels-inspector-variable-tests)

    add_executable(
        pixels-customer-fixture-generator
        tools/generate_customer_fixture.cpp)
    target_link_libraries(
        pixels-customer-fixture-generator PRIVATE pixels-inspector)
    add_test(
        NAME pixels-customer-fixture-generator-test
        COMMAND pixels-customer-fixture-generator
                "${CMAKE_CURRENT_BINARY_DIR}/customer-test.pxl")
endif()

if(EMSCRIPTEN)
    add_executable(pixels-inspector-wasm tools/wasm_entry.cpp)
    target_link_libraries(pixels-inspector-wasm PRIVATE pixels-inspector)
    set_target_properties(pixels-inspector-wasm PROPERTIES SUFFIX ".cjs")
    foreach(target
            libprotobuf-lite
            pixels-inspector-proto
            pixels-format
            pixels-inspector
            pixels-inspector-wasm)
        target_compile_options(${target} PRIVATE
            "SHELL:-ffile-prefix-map=${PIXELS_SOURCE_ROOT}=third_party/pixels")
    endforeach()
    target_link_options(pixels-inspector-wasm PRIVATE
        "SHELL:--no-entry"
        "SHELL:-sFILESYSTEM=0"
        "SHELL:-sMODULARIZE=1"
        "SHELL:-sEXPORT_NAME=createPixelsInspector"
        "SHELL:-sENVIRONMENT=web,worker,node"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sINITIAL_MEMORY=16777216"
        "SHELL:-sMAXIMUM_MEMORY=268435456"
        "SHELL:-sINCOMING_MODULE_JS_API=['wasmBinary','locateFile','print','printErr','noInitialRun']"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['HEAPU8']"
        "SHELL:-sEXPORTED_FUNCTIONS=['_malloc','_free','_pixels_inspector_abi_version','_pixels_inspector_capabilities_size','_pixels_inspector_copy_capabilities','_pixels_inspector_create','_pixels_inspector_destroy','_pixels_inspector_begin_metadata','_pixels_inspector_begin_row_group','_pixels_inspector_begin_plain_long_page','_pixels_inspector_begin_page','_pixels_inspector_begin_rows','_pixels_inspector_begin_filter','_pixels_inspector_begin_scan','_pixels_inspector_copy_scan_progress','_pixels_inspector_next_range','_pixels_inspector_supply_range','_pixels_inspector_result_size','_pixels_inspector_copy_result','_pixels_inspector_error_size','_pixels_inspector_copy_error','_pixels_inspector_cancel']")
endif()
