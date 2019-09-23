TEMPLATE = app
TARGET = lafrpc
CONFIG += console
QT = core network
#CONFIG -= app_bundle
SOURCES = tests/simple_test.cpp \
    tests/sendfile.cpp

include(lafrpc.pri)
