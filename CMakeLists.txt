cmake_minimum_required(VERSION 3.10)
project(app-usage-monitor VERSION 1.3.2 LANGUAGES CXX)

# 版本号只在 CMake 里维护，C++ 侧用 APP_USAGE_VERSION 引用
add_definitions(-DAPP_USAGE_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

find_package(Qt5 5.11 REQUIRED COMPONENTS Widgets Sql DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DDE_DOCK REQUIRED dde-dock)
pkg_check_modules(X11 REQUIRED x11)

add_library(app-usage-monitor MODULE
    src/appusagemonitorplugin.cpp
    src/appusagemonitorplugin.h
    src/usagetracker.cpp
    src/usagetracker.h
    src/x11windowinfo.cpp
    src/x11windowinfo.h
    src/idledetector.cpp
    src/idledetector.h
    src/appregistry.cpp
    src/appregistry.h
    src/usagestorage.cpp
    src/usagestorage.h
    src/usagepopupwidget.cpp
    src/usagepopupwidget.h
    src/usagechart.cpp
    src/usagechart.h
    src/contextswitch.cpp
    src/contextswitch.h
    src/chattitleocr.cpp
    src/chattitleocr.h
    src/switchstats.cpp
    src/switchstats.h
    src/addeventdialog.cpp
    src/addeventdialog.h
    src/restreminder.cpp
    src/restreminder.h
    src/restnotifier.cpp
    src/restnotifier.h
    src/usageentry.h
)

target_include_directories(app-usage-monitor PRIVATE
    ${DDE_DOCK_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
)

target_link_libraries(app-usage-monitor PRIVATE
    Qt5::Widgets
    Qt5::Sql
    Qt5::DBus
    ${X11_LIBRARIES}
)

# 输出 libapp-usage-monitor.so，与任务栏插件命名规范一致
set_target_properties(app-usage-monitor PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)

# 导出工具：把数据库导成 CSV（不依赖任务栏，单独装到 /usr/bin）
add_executable(app-usage-monitor-export tools/export.cpp)
target_link_libraries(app-usage-monitor-export PRIVATE Qt5::Core Qt5::Sql)
set_target_properties(app-usage-monitor-export PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)
