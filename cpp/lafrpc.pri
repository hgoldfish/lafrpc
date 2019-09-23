QT += core network
CONFIG += c++11

SOURCES += $$PWD/src/peer.cpp \
    $$PWD/src/rpc.cpp \
    $$PWD/src/serialization.cpp \
    $$PWD/src/base.cpp \
    $$PWD/src/transport.cpp \
    $$PWD/src/tran_crypto.cpp

HEADERS += $$PWD/lafrpc.h \
    $$PWD/include/peer.h \
    $$PWD/include/rpc.h \
    $$PWD/include/utils.h \
    $$PWD/include/serialization.h \
    $$PWD/include/base.h \
    $$PWD/include/transport.h \
    $$PWD/include/rpc_p.h \
    $$PWD/include/tran_crypto.h

include(qtnetworkng/qtnetworkng.pri)

HEADERS += $$PWD/qtnetworkng/contrib/data_channel.h \
    $$PWD/qtnetworkng/contrib/data_pack.h

SOURCES += $$PWD/qtnetworkng/contrib/data_channel.cpp
