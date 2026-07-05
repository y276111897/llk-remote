cmake_minimum_required(VERSION 3.20)

project(llk_remote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(llk_host
    src/host_main.cpp
)

target_include_directories(llk_host PRIVATE include)
target_compile_definitions(llk_host PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(llk_host PRIVATE
    ws2_32
    user32
    shell32
)

add_executable(llk_viewer WIN32
    src/viewer_main.cpp
)

target_include_directories(llk_viewer PRIVATE include)
target_compile_definitions(llk_viewer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(llk_viewer PRIVATE
    ws2_32
    user32
    gdi32
    d3d11
    dxgi
    d3dcompiler
    shell32
)
