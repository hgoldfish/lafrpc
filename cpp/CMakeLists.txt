cmake_minimum_required(VERSION 3.1.0...3.28 FATAL_ERROR)

project(lafrpc)

# 网络库提供方：默认使用独立的 qtnetworkng 项目（qtnetworkng/qtnetworkng.h 或同级检出）；
# 设为 ON 后改用 qtng 仓库的 Qt binding（qtng/qt），它同样提供向后兼容的 qtnetworkng 目标。
option(LAFRPC_USE_QTNG "Use the Qt binding from the qtng repo instead of the standalone qtnetworkng project" OFF)

if(LAFRPC_USE_QTNG)
    # qtng 的 Qt binding 提供向后兼容的 qtnetworkng 库，取代已弃用的独立 qtnetworkng 项目。
    # 优先使用同级 qtng 仓库；否则回退到本地 qtng 子模块（如 git clone --recursive 的独立检出）。
    # 用 add_subdirectory 而不是 include 引入：include 会共享目录作用域，qtng 里对
    # CMAKE_CXX_STANDARD / CMAKE_AUTOMOC / cmake_minimum_required 的全局修改会泄漏并覆盖
    # 本项目的设置（如 Qt6 下把 C++11 抬成 C++17）；add_subdirectory 则把这些变量隔离在子目录中。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../qtng/qt/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../qtng/qt" qtng-qt EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qtng/qt/CMakeLists.txt")
        add_subdirectory(qtng/qt qtng-qt EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "qtng Qt binding (qtng/qt) not found. "
            "Expected at ${CMAKE_CURRENT_SOURCE_DIR}/../../qtng/qt/CMakeLists.txt (sibling qtng checkout) "
            "or ${CMAKE_CURRENT_SOURCE_DIR}/qtng/qt/CMakeLists.txt (qtng submodule).")
    endif()

    # Qt 主版本跟随 qtng 的探测结果（QTNG_QT_VERSION 决定 Qt5/Qt6），不自己探测。
    if(NOT DEFINED QTNG_QT_VERSION_MAJOR)
        message(FATAL_ERROR "qtng/qt did not report its Qt version (QTNG_QT_VERSION_MAJOR not set).")
    endif()
    set(LAFRPC_QT_VERSION_MAJOR "${QTNG_QT_VERSION_MAJOR}")
else()
    # 独立的 qtnetworkng 项目，只支持 Qt5。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../qtnetworkng/qtnetworkng.h")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../qtnetworkng/" qtnetworkng EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qtnetworkng/qtnetworkng.h")
        add_subdirectory(qtnetworkng qtnetworkng EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "qtnetworkng not found. "
            "Expected at ${CMAKE_CURRENT_SOURCE_DIR}/../../qtnetworkng/ (sibling qtnetworkng checkout) "
            "or ${CMAKE_CURRENT_SOURCE_DIR}/qtnetworkng (qtnetworkng submodule).")
    endif()
    set(LAFRPC_QT_VERSION_MAJOR 5)
endif()

find_package(Qt${LAFRPC_QT_VERSION_MAJOR}Core CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)

option(LAFRPC_BUILD_TESTS OFF)
set(CMAKE_AUTOMOC ON)
if (CMAKE_VERSION VERSION_LESS "3.1")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    else()
        message(SEND_ERROR "Please update your cmake to enable c++11.")
    endif ()
else ()
    set (CMAKE_CXX_STANDARD 11)
endif ()


set(LAFRPC_SRC
    src/peer.cpp
    src/rpc.cpp
    src/serialization.cpp
    src/base.cpp
    src/transport.cpp
    src/sendfile.cpp
    src/senddir.cpp
)

set(LAFRPC_INCLUDE
    include/peer.h
    include/rpc.h
    include/utils.h
    include/serialization.h
    include/serialization_p.h
    include/base.h
    include/transport.h
    include/rpc_p.h
    include/sendfile.h
    include/senddir.h
)

# Fix Qt-static cmake BUG
# https://bugreports.qt.io/browse/QTBUG-38913
set(_lafrpc_qt_install_prefix "${_qt${LAFRPC_QT_VERSION_MAJOR}Core_install_prefix}")
if(EXISTS ${_lafrpc_qt_install_prefix}/lib/libQt${LAFRPC_QT_VERSION_MAJOR}Core.a AND NOT EXISTS ${_lafrpc_qt_install_prefix}/bin/Qt${LAFRPC_QT_VERSION_MAJOR}Core.dll)
    message("Static Qt is detected.")
    link_directories(${_lafrpc_qt_install_prefix}/lib/)
endif()


add_library(lafrpc STATIC ${LAFRPC_SRC} ${LAFRPC_INCLUDE})
target_link_libraries(lafrpc PUBLIC qtnetworkng)
target_include_directories(lafrpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(LAFRPC_BUILD_TESTS)
    find_package(Qt${LAFRPC_QT_VERSION_MAJOR}Test CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)
    add_executable(rpctest tests/simple_test.cpp)
    target_link_libraries(rpctest Qt${LAFRPC_QT_VERSION_MAJOR}::Core lafrpc)

    add_executable(sendfiletest tests/sendfile.cpp)
    target_link_libraries(sendfiletest PRIVATE Qt${LAFRPC_QT_VERSION_MAJOR}::Core lafrpc)

    add_executable(senddirtest tests/senddir_test.cpp)
    target_link_libraries(senddirtest PRIVATE Qt${LAFRPC_QT_VERSION_MAJOR}::Test Qt${LAFRPC_QT_VERSION_MAJOR}::Core lafrpc)
endif()
