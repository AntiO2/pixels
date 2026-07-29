if(NOT DEFINED PIXELS_CPP_ROOT OR NOT DEFINED PIXELS_INSPECTOR_ROOT)
    message(FATAL_ERROR "portable dependency scan requires source roots")
endif()

file(GLOB_RECURSE PORTABLE_SOURCES
    "${PIXELS_CPP_ROOT}/pixels-core/include/format/*.h"
    "${PIXELS_CPP_ROOT}/pixels-core/lib/format/*.cpp"
    "${PIXELS_INSPECTOR_ROOT}/include/*.h"
    "${PIXELS_INSPECTOR_ROOT}/lib/*.cpp")

if(NOT PORTABLE_SOURCES)
    message(FATAL_ERROR "portable dependency scan found no sources")
endif()

set(FORBIDDEN_PATTERNS
    "physical/"
    "PhysicalReader"
    "Storage"
    "Scheduler"
    "BufferPool"
    "liburing"
    "io_uring"
    "duckdb"
    "ConfigFactory"
    "immintrin"
    "fcntl\\.h"
    "pthread")

foreach(SOURCE_FILE IN LISTS PORTABLE_SOURCES)
    file(READ "${SOURCE_FILE}" SOURCE_CONTENT)
    foreach(FORBIDDEN_PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(SOURCE_CONTENT MATCHES "${FORBIDDEN_PATTERN}")
            message(FATAL_ERROR
                "forbidden portable dependency '${FORBIDDEN_PATTERN}' "
                "found in ${SOURCE_FILE}")
        endif()
    endforeach()
endforeach()

message(STATUS
    "portable dependency scan passed for ${PORTABLE_SOURCES}")
