cmake_minimum_required(VERSION 3.16)
project(c_web_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CWS_BUILD_TESTS  "Build unit tests"                  OFF)
option(CWS_USE_ASAN     "Enable AddressSanitizer"            OFF)
option(CWS_USE_UBSAN    "Enable UndefinedBehaviorSanitizer"  OFF)
option(CWS_USE_TSAN     "Enable ThreadSanitizer"             OFF)
option(CWS_BUILD_BENCH  "Generate benchmark targets/scripts" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CWS_SRC
    src/server/server.c
    src/http/parser.c
    src/http/request.c
    src/http/response.c
    src/routing/router.c
    src/utils/errors.c
    src/utils/log.c
    src/utils/cpu.c
    src/utils/config.c
    src/utils/metrics.c
)

add_library(cws STATIC ${CWS_SRC})
target_include_directories(cws PUBLIC include)
target_link_libraries(cws PUBLIC pthread)

target_compile_options(cws PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wshadow -Wstrict-prototypes
    -Wno-unused-parameter
    -fstack-protector-strong
)

# Architecture tuning for Intel Xeon / AMD EPYC server CPUs.
# -march=x86-64-v3 enables AVX2, BMI1/2, FMA, MOVBE — baseline on modern
# server silicon since Haswell/Excavator while remaining portable on
# both Intel Xeon Scalable and AMD EPYC in mainstream deployment.
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

add_executable(cws_main main.c)
target_link_libraries(cws_main PRIVATE cws)

install(TARGETS cws cws_main
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

enable_testing()
