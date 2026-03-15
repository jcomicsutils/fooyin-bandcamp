cmake_minimum_required(VERSION 3.18)

project(
    bandcampinput
    VERSION 0.0.0
    DESCRIPTION "Bandcamp streaming plugin for fooyin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets Concurrent)

# minimp3 — single-header MP3 decoder (MIT licensed, no extra deps)
include(FetchContent)
FetchContent_Declare(
    minimp3
    GIT_REPOSITORY https://github.com/lieff/minimp3.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minimp3)

configure_file(src/bandcamp.json.in ${CMAKE_CURRENT_BINARY_DIR}/bandcamp.json @ONLY)

create_fooyin_plugin(
    bandcampinput
    JSON_IN src/bandcamp.json.in
    DEPENDS
        Fooyin::Core
        Fooyin::Gui
        Qt6::Network
        Qt6::Widgets
        Qt6::Concurrent
    SOURCES
        src/bandcampplugin.h
        src/bandcampplugin.cpp
        src/bandcampfetcher.h
        src/bandcampfetcher.cpp
        src/bandcampinput.h
        src/bandcampinput.cpp
        src/bandcampdialog.h
        src/bandcampdialog.cpp
)

target_include_directories(bandcampinput PRIVATE
    src/
    ${minimp3_SOURCE_DIR}
)
