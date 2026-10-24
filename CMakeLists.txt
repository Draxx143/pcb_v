cmake_minimum_required(VERSION 3.16)
project(PcbAiViewer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(PcbAiViewer
    main.cpp
    MainWindow.h
    MainWindow.cpp
    OleParser.h
    OleParser.cpp
)

target_link_libraries(PcbAiViewer PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
