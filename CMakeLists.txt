# MHFU-Android -- Monster Hunter Freedom Unite (EU, ULES01213), statically
# recompiled with psprecomp.
#
# Builds:
#   - psprecomp (runtime library + the allegrexrecomp tool), from the submodule
#   - mhfu_host, the desktop bring-up host, once work/gen/ holds emitted C
#
# work/gen/ is produced from your own decrypted EBOOT by scripts/setup.ps1 and
# is never committed.

cmake_minimum_required(VERSION 3.16)
project(mhfu_android C)

set(PSPRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/psprecomp" CACHE PATH "psprecomp checkout")
set(MHFU_GEN_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/work/gen"  CACHE PATH "allegrexrecomp emit output")
set(MHFU_OVL_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/work/gen_ovl" CACHE PATH "recompiled overlays")

if(NOT EXISTS "${PSPRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "psprecomp not found at ${PSPRECOMP_DIR}. Run:\n"
        "    git submodule update --init --recursive")
endif()

# psprecomp only got as far as gameplay in its own test game with real guest
# threads, so they are on by default here.
set(PSPRECOMP_REAL_THREADS ON CACHE BOOL "Enable cooperative host-thread scheduling")

add_subdirectory("${PSPRECOMP_DIR}" psprecomp)

# --- tools ------------------------------------------------------------------------

# Wraps an overlay the host saved as an ELF for allegrexrecomp (setup.ps1 runs it).
add_executable(mhfu_ovl tools/mhfu_ovl.c)
set_target_properties(mhfu_ovl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_definitions(mhfu_ovl PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The room network (ad hoc multiplayer, see host/net/proto.h): a library the
# host links, the standalone hub for a server, and a check that runs it end to
# end without the game.
add_library(mhfu_net STATIC host/net/sys.c host/net/hub.c host/net/client.c)
target_include_directories(mhfu_net PUBLIC host)
set_target_properties(mhfu_net PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(WIN32)
    target_link_libraries(mhfu_net PUBLIC ws2_32 advapi32)
    target_compile_definitions(mhfu_net PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mhfu_net PUBLIC Threads::Threads)
endif()
add_executable(mhfu_hub tools/mhfu_hub.c)
target_link_libraries(mhfu_hub PRIVATE mhfu_net)
add_executable(mhfu_netcheck tools/mhfu_netcheck.c)
target_link_libraries(mhfu_netcheck PRIVATE mhfu_net)
set_target_properties(mhfu_hub mhfu_netcheck PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# --- the host -------------------------------------------------------------------

file(GLOB MHFU_GEN_SOURCES CONFIGURE_DEPENDS "${MHFU_GEN_DIR}/*.c")

if(NOT MHFU_GEN_SOURCES)
    message(STATUS
        "No recompiled code in ${MHFU_GEN_DIR}: building psprecomp only. "
        "Run scripts/setup.ps1, then configure again.")
    return()
endif()

list(LENGTH MHFU_GEN_SOURCES MHFU_GEN_COUNT)
message(STATUS "mhfu_host: ${MHFU_GEN_COUNT} generated source files")

# Overlays: one directory per recompiled overlay, plus the table that names
# them. Before any exist, an empty table.
file(GLOB MHFU_OVL_SOURCES CONFIGURE_DEPENDS "${MHFU_OVL_DIR}/*/*.c")
if(EXISTS "${MHFU_OVL_DIR}/overlays.c")
    list(APPEND MHFU_OVL_SOURCES "${MHFU_OVL_DIR}/overlays.c")
else()
    set(MHFU_OVL_SOURCES host/overlays_none.c)
endif()
list(APPEND MHFU_GEN_SOURCES ${MHFU_OVL_SOURCES})

add_executable(mhfu_host host/main.c host/hle_mhfu.c host/input.c host/window.c host/libfont.c host/utility.c
               host/adhoc.c host/lobby.c host/audio.c host/atrac.c host/mpeg.c ${MHFU_GEN_SOURCES})
target_include_directories(mhfu_host PRIVATE "${MHFU_GEN_DIR}" host)
target_link_libraries(mhfu_host PRIVATE psprecomp mhfu_net)
if(WIN32)
    target_link_libraries(mhfu_host PRIVATE user32 gdi32)
endif()

# Function-entry tracing: the report then names the last function reached and
# dumps the recent call trail. Slower, but bring-up needs it.
option(MHFU_TRACE "Record every recompiled function entry" ON)
if(MHFU_TRACE)
    target_compile_definitions(mhfu_host PRIVATE PSPRECOMP_TRACE)
endif()
set_target_properties(mhfu_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# Generated code is machine output: no warnings, and build it in parallel.
if(MSVC)
    target_compile_options(mhfu_host PRIVATE /MP)
    target_compile_definitions(mhfu_host PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_source_files_properties(${MHFU_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${MHFU_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()
