QT += core network sql testlib
QT -= gui
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = worker_lifecycle_test

INCLUDEPATH += ..
SOURCES += worker_lifecycle_test.cpp \
           ../mytcpserver.cpp \
           ../mytcpsocket.cpp \
           ../opedb.cpp \
           ../protocol.cpp
HEADERS += ../mytcpserver.h \
           ../mytcpsocket.h \
           ../opedb.h \
           ../protocol.h
