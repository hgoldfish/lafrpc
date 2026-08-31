cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)
project(lafrpc_qtng_rpc_interop LANGUAGES CXX)

# wire-compatibility test: legacy lafrpc (Qt) <-> qtng/rpc (std).
#
# Two separate executables talk over a real TCP socket, mirroring the
# production layout (pbook_server built on plain std qtng; the PC client
# built on the Qt binding):
#
#   interop_std_server  — plain std qtng + qtng/rpc, serves "std" service
#   interop_qt_client   — qtnetworkng (Qt binding) + lafrpc, serves "demo"
#                         and calls into the std server
#
#   cmake -B build -DCMAKE_CXX_COMPILER=g++-13 -DLAFRPC_USE_QTNG=ON
#   cmake --build build
#   ./build/interop_std_server 17942 &
#   ./build/interop_qt_client 17942
#
# Namespace/target layout:
#   * lafrpc configures first. LAFRPC_USE_QTNG=ON makes it pull in qtng/qt,
#     whose `if(NOT TARGET qtng)` guard fires and builds the core compiled
#     into namespace qtng_core, linked privately into libqtnetworkng.a.
#   * qtng/rpc is added AFTER lafrpc. Its guard sees TARGET qtng already
#     exists (the qtng_core-remapped one) and links it; so qtng_rpc must be
#     compiled with the same `qtng=qtng_core` remapping, making its symbols
#     qtng_core::rpc::* and its core calls resolve inside libqtng.a
#     (the remapped core). It only feeds interop_std_server.
#   * The two executables exchange bytes over a real socket, so they never
#     share C++ symbols with each other.

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QTNG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../qtng" CACHE PATH "path to the qtng repo")
set(LAFRPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "path to lafrpc/cpp")

# --- qt side first (owns the one and only `qtng` target, remapped) ---
set(LAFRPC_USE_QTNG ON CACHE BOOL "" FORCE)
add_subdirectory("${LAFRPC_DIR}" lafrpc EXCLUDE_FROM_ALL)

find_package(Qt5 COMPONENTS Core QUIET)
if(Qt5_FOUND)
    set(QT_LIB Qt5::Core)
else()
    find_package(Qt6 COMPONENTS Core REQUIRED)
    set(QT_LIB Qt6::Core)
endif()

add_executable(interop_qt_client qt_client.cpp)
target_include_directories(interop_qt_client PRIVATE "${LAFRPC_DIR}")
target_link_libraries(interop_qt_client PRIVATE lafrpc qtnetworkng ${QT_LIB})

# --- std side (reuses the same remapped `qtng` core target) ---
add_subdirectory("${QTNG_ROOT}/rpc" qtng-rpc EXCLUDE_FROM_ALL)
target_compile_definitions(qtng_rpc PRIVATE qtng=qtng_core)

add_executable(interop_std_server std_server.cpp)
target_compile_definitions(interop_std_server PRIVATE qtng=qtng_core)
target_include_directories(interop_std_server PRIVATE
    "${QTNG_ROOT}/include"
    "${QTNG_ROOT}/rpc/include"
)
target_link_libraries(interop_std_server PRIVATE qtng qtng_rpc pthread)
