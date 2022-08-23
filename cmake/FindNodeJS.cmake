cmake_minimum_required(VERSION 3.22)

find_program(NodeJS_EXECUTABLE node nodejs)

if (NodeJS_EXECUTABLE)
    execute_process(COMMAND "${NodeJS_EXECUTABLE}" --version
                    OUTPUT_VARIABLE NodeJS_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "v" "" NodeJS_VERSION ${NodeJS_VERSION})
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    NodeJS
    REQUIRED_VARS NodeJS_EXECUTABLE
    VERSION_VAR NodeJS_VERSION
    HANDLE_VERSION_RANGE
    HANDLE_COMPONENTS
)

if (NodeJS_FOUND AND NOT TARGET NodeJS::Interpreter)
    add_executable(NodeJS::Interpreter IMPORTED)
    set_target_properties(NodeJS::Interpreter PROPERTIES IMPORTED_LOCATION "${NodeJS_EXECUTABLE}")
endif ()
