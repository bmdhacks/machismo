cmake_minimum_required(VERSION 3.16)
project(machismo C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

option(BUILD_WRAPGEN "Build the wrapgen utility" ON)
option(BUILD_TESTS "Build test fixtures" ON)

# --- machismo loader executable ---

set(MACHISMO_SOURCES
    src/machismo.c
    src/commpage.c
    src/resolver.c
    src/trampoline.c
    src/gdb_jit.c
    src/patcher.c
    src/config.c
    src/bgfx_shim.c
    src/eh_frame.c
    src/dylib_loader.c
    src/lse_emul.c
    src/elfcalls/elfcalls.c
    src/elfcalls/threads.c
)

add_executable(machismo ${MACHISMO_SOURCES})

target_include_directories(machismo PRIVATE src)
target_compile_options(machismo PRIVATE -Wall -O2 -g)
target_compile_definitions(machismo PRIVATE
    INSTALL_PREFIX=""
    SYSTEM_ROOT=""
    _GNU_SOURCE
)
target_link_libraries(machismo PRIVATE dl rt pthread m)

# --- libsystem_shim.so ---

add_library(system_shim SHARED src/shim/libsystem_shim.c)
set_target_properties(system_shim PROPERTIES
    OUTPUT_NAME "system_shim"
    PREFIX "lib"
)
target_compile_options(system_shim PRIVATE -Wall -O2 -g)
target_link_libraries(system_shim PRIVATE m pthread dl)
target_link_options(system_shim PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/shim/libsystem_shim.ver
)

# --- wrapgen utility ---

if(BUILD_WRAPGEN)
    add_executable(wrapgen tools/wrapgen.cpp)
    target_compile_options(wrapgen PRIVATE -Wall -O2 -g)
    target_link_libraries(wrapgen PRIVATE dl)
endif()

# --- test_lse_emul ---

if(BUILD_TESTS)
    add_executable(test_lse_emul tests/test_lse_emul.c src/lse_emul.c)
    target_include_directories(test_lse_emul PRIVATE src)
    target_compile_options(test_lse_emul PRIVATE -Wall -O2 -g)
    target_compile_definitions(test_lse_emul PRIVATE _GNU_SOURCE)
    target_link_libraries(test_lse_emul PRIVATE pthread)
endif()

# --- install ---

install(TARGETS machismo RUNTIME DESTINATION bin)
install(TARGETS system_shim LIBRARY DESTINATION lib)
if(BUILD_WRAPGEN)
    install(TARGETS wrapgen RUNTIME DESTINATION bin)
endif()
