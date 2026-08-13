cmake_minimum_required(VERSION 4.4)
project(asmview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 CONFIG REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(asmview
        src/main.cpp
        src/analysis.cpp
        src/analysis.hpp
        src/breadcrumb.cpp
        src/breadcrumb.hpp
        src/ribbon.cpp
        src/ribbon.hpp
        src/code_view.cpp
        src/code_view.hpp
        src/highlighters.cpp
        src/highlighters.hpp
        src/main_window.cpp
        src/main_window.hpp
        src/theme.cpp
        src/theme.hpp
)

target_link_libraries(asmview PRIVATE Qt6::Widgets)
