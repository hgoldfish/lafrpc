cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)

project(lafrpc)

if(ANDROID)
    find_package(Qt5Core CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)
else()
    find_package(Qt5Core REQUIRED)
endif()

option(LAFRPC_BUILD_TESTS OFF)
set(CMAKE_AUTOMOC ON)

set(LAFRPC_SRC
    src/peer.cpp
    src/rpc.cpp
    src/serialization.cpp
    src/base.cpp
    src/transport.cpp
    src/sendfile.cpp
)

set(LAFRPC_INCLUDE
    include/peer.h
    include/rpc.h
    include/utils.h
    include/serialization.h
    include/base.h
    include/transport.h
    include/rpc_p.h
    include/sendfile.h
)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    add_subdirectory(../../qtnetworkng/ qtnetworkng)
else()
    add_subdirectory(qtnetworkng)
endif()

# Fix Qt-static cmake BUG
# https://bugreports.qt.io/browse/QTBUG-38913
if(EXISTS ${_qt5Core_install_prefix}/lib/libQt5Core.a AND NOT EXISTS ${_qt5Core_install_prefix}/bin/Qt5Core.dll)
    message("Static Qt is detected.")
    link_directories(${_qt5Core_install_prefix}/lib/)
endif()


add_library(lafrpc STATIC ${LAFRPC_SRC} ${LAFRPC_INCLUDE})
target_link_libraries(lafrpc PUBLIC qtnetworkng)
target_include_directories(lafrpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(LAFRPC_BUILD_TESTS)
    add_executable(rpctest tests/simple_test.cpp)
    target_link_libraries(rpctest Qt5::Core lafrpc)

    add_executable(sendfiletest tests/sendfile.cpp)
    target_link_libraries(sendfiletest PRIVATE Qt5::Core lafrpc)
endif()
