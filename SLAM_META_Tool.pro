QT += core gui widgets sql network mqtt

CONFIG += c++11
TEMPLATE = app
TARGET = SLAM_META_Tool

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    batchprogressdialog.cpp \
    batchrunner.cpp \
    historydatabase.cpp \
    configuploader.cpp \
    configuploaddialog.cpp

HEADERS += \
    mainwindow.h \
    batchprogressdialog.h \
    batchrunner.h \
    historydatabase.h \
    tasktypes.h \
    configuploader.h \
    configuploaddialog.h

win32: LIBS += -lwlanapi -lole32

DESTDIR = bin

