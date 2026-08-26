#!/bin/sh
# 宿主机交叉编译 Qt V4L2 测试相机（不依赖容器、不依赖 qmake）
set -e
R=/home/zzh/workspace/rk3588_project/buildroot/output/alientek_rk3588
SYSROOT=$R/host/aarch64-buildroot-linux-gnu/sysroot
GXX=$R/host/bin/aarch64-buildroot-linux-gnu-g++
$GXX --sysroot=$SYSROOT   -I$SYSROOT/usr/include/qt5   -I$SYSROOT/usr/include/qt5/QtWidgets   -I$SYSROOT/usr/include/qt5/QtGui   -I$SYSROOT/usr/include/qt5/QtCore   -fPIC -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB -pthread   main.cpp   -L$SYSROOT/usr/lib -lQt5Widgets -lQt5Gui -lQt5Core   -o qt_test_camera
