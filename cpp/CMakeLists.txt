CMAKE_MINIMUM_REQUIRED(VERSION 3.1.0 FATAL_ERROR)

PROJECT(laf_rpc)

IF(ANDROID)
    FIND_PACKAGE(Qt5Core CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)
ELSE()
    FIND_PACKAGE(Qt5Core REQUIRED)
ENDIF()

SET(CMAKE_AUTOMOC ON)
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

SET(LAFRPC_SRC
    src/peer.cpp
    src/rpc.cpp
    src/serialization.cpp
    src/base.cpp
    src/transport.cpp
    src/tran_crypto.cpp
    src/sendfile.cpp
    qtnetworkng/contrib/data_channel.cpp
)

SET(LAFRPC_INCLUDE
    include/peer.h
    include/rpc.h
    include/utils.h
    include/serialization.h
    include/base.h
    include/transport.h
    include/rpc_p.h
    include/tran_crypto.h
    include/sendfile.h
    qtnetworkng/contrib/data_channel.h
)

ADD_SUBDIRECTORY(qtnetworkng)
ADD_SUBDIRECTORY(qmsgpack)

# Fix Qt-static cmake BUG
# https://bugreports.qt.io/browse/QTBUG-38913
IF((EXISTS ${_qt5Core_install_prefix}/lib/libQt5Core.so) OR
   (EXISTS ${_qt5Core_install_prefix}/lib/Qt5Core.dll) OR
   (EXISTS ${_qt5Core_install_prefix}/lib64/libQt5Core.so) OR
   (EXISTS ${_qt5Core_install_prefix}/lib64/Qt5Core.dll))
    MESSAGE("Shared Qt is detected.")
ELSE()
    MESSAGE("Static Qt is detected.")
    LINK_DIRECTORIES(${_qt5Core_install_prefix}/lib/)
ENDIF()

ADD_LIBRARY(laf_rpc STATIC ${LAFRPC_SRC} ${LAFRPC_INCLUDE})
TARGET_LINK_LIBRARIES(laf_rpc qtnetworkng qmsgpack)

add_executable(rpctest tests/simple_test.cpp)
target_link_libraries(rpctest Qt5::Core laf_rpc)

add_executable(sendfiletest tests/sendfile.cpp)
target_link_libraries(sendfiletest PRIVATE Qt5::Core laf_rpc)
