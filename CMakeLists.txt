cmake_minimum_required(VERSION 3.16)
project(c_web_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CWS_BUILD_TESTS   "Build unit tests"                   OFF)
option(CWS_BUILD_EXAMPLES "Build examples"                      ON)
option(CWS_USE_ASAN      "Enable AddressSanitizer"             OFF)
option(CWS_USE_UBSAN     "Enable UndefinedBehaviorSanitizer"   OFF)
option(CWS_USE_TSAN      "Enable ThreadSanitizer"               OFF)
option(CWS_BUILD_BENCH   "Generate benchmark targets/scripts"  OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CWS_SRC
    src/app/app.c
    src/server/server.c
    src/http/parser.c
    src/http/request.c
    src/http/response.c
    src/routing/router.c
    src/routing/middleware.c
    src/routing/middleware_builtins.c
    src/utils/errors.c
    src/utils/log.c
    src/utils/cpu.c
    src/utils/config.c
    src/utils/metrics.c
    src/utils/env.c
)

add_library(cws STATIC ${CWS_SRC})
target_include_directories(cws
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(cws PUBLIC pthread)

set_target_properties(cws PROPERTIES
    OUTPUT_NAME          cws
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Strict warnings only for the library build, not imposed on consumers.
target_compile_options(cws PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wshadow -Wstrict-prototypes
    -Wno-unused-parameter
    -fstack-protector-strong
)

# Architecture tuning for Intel Xeon / AMD EPYC server CPUs.
# Applied as PUBLIC so that consumers benefit from the same ISA baseline
# when linking cws (the .so/.a was compiled for x86-64-v3 anyway).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cws PUBLIC -march=x86-64-v3 -mtune=native -O3)
    target_compile_options(cws PUBLIC -fno-plt -fno-semantic-interposition)
    target_compile_options(cws PUBLIC -falign-functions=32 -falign-loops=32)
    target_compile_options(cws PUBLIC -funroll-loops)
endif()

if(CWS_USE_ASAN)
    target_compile_options(cws PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(cws   PUBLIC -fsanitize=address)
endif()
if(CWS_USE_UBSAN)
    target_compile_options(cws PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(cws   PUBLIC -fsanitize=undefined)
endif()
if(CWS_USE_TSAN)
    target_compile_options(cws PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(cws   PUBLIC -fsanitize=thread)
endif()

# Examples
if(CWS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Install + export + pkg-config
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS cws
        EXPORT cwsTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT cwsTargets
        FILE cwsTargets.cmake
        NAMESPACE cws::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cws)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cwsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cwsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cws
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cwsConfigVersion.cmake
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cwsConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cwsConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cws)

# pkg-config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cws.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/cws.pc
    @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cws.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

enable_testing()
