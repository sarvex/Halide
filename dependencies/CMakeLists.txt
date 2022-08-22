##
# Standard CMake find-module dependencies
##

set(THREADS_PREFER_PTHREAD_FLAG YES)
find_package(Threads REQUIRED)
set_target_properties(Threads::Threads PROPERTIES IMPORTED_GLOBAL TRUE)

##
# Third-party dependencies in their own subdirectories
##

add_subdirectory(llvm)

add_subdirectory(jpeg)
add_subdirectory(png)

if (TARGET_SPIRV)
    add_subdirectory(spirv)
endif()

# Needs cache vars set by llvm, do not reorder.
add_subdirectory(wasm)
