cmake_minimum_required(VERSION 3.22)

find_library(V8_LIBRARY v8_monolith v8)
find_path(V8_INCLUDE_DIR v8.h libplatform/libplatform.h)

if (EXISTS "${V8_INCLUDE_DIR}/v8-version.h")
    set(_V8_version_macros V8_MAJOR_VERSION V8_MINOR_VERSION V8_BUILD_NUMBER V8_PATCH_LEVEL)
    file(STRINGS "${V8_INCLUDE_DIR}/v8-version.h" _V8_version_header)
    foreach (line IN LISTS _V8_version_header)
        if (line MATCHES "^#define +([A-Z0-9_]+) +([0-9]+)$")
            if (CMAKE_MATCH_1 IN_LIST _V8_version_macros)
                set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
            endif ()
        endif ()
    endforeach ()
    set(V8_VERSION "${V8_MAJOR_VERSION}.${V8_MINOR_VERSION}.${V8_BUILD_NUMBER}.${V8_PATCH_LEVEL}")
    unset(_V8_version_macros)
    unset(_V8_version_header)
else ()
    set(V8_VERSION "")
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    V8
    REQUIRED_VARS V8_LIBRARY V8_INCLUDE_DIR
    VERSION_VAR V8_VERSION
    HANDLE_VERSION_RANGE
    HANDLE_COMPONENTS
)

if (V8_FOUND AND NOT TARGET V8::V8)
    add_library(V8::V8 STATIC IMPORTED)
    set_target_properties(V8::V8 PROPERTIES IMPORTED_LOCATION "${V8_LIBRARY}")
    target_include_directories(V8::V8 INTERFACE "${V8_INCLUDE_DIR}")
endif ()
