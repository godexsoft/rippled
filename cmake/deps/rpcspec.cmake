# Try Conan-managed package first (when built via conan install + cmake toolchain).
# Fall back to sibling directory for direct cmake builds.
find_package(xrpl-rpc-spec CONFIG QUIET)
if(NOT xrpl-rpc-spec_FOUND)
    add_library(rpcspec INTERFACE)
    add_library(rpcspec::rpcspec ALIAS rpcspec)
    target_include_directories(
        rpcspec
        INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../xrpl-rpc-spec/include"
    )
    target_compile_features(rpcspec INTERFACE cxx_std_23)
endif()
# RPCSPEC_IS_XRPLD=1 is set here for direct cmake builds; Conan-based builds
# additionally get it from the generated conan_toolchain.cmake.
if(NOT DEFINED RPCSPEC_IS_XRPLD)
    add_compile_definitions(RPCSPEC_IS_XRPLD=1)
endif()
