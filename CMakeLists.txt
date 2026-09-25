cmake_minimum_required(VERSION 3.20)

project(NppLatexPreview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WEBVIEW2_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2"
)

set(WEBVIEW2_INCLUDE_DIR
    "${WEBVIEW2_ROOT}/include"
)

set(WEBVIEW2_LIB
    "${WEBVIEW2_ROOT}/x64/WebView2Loader.dll.lib"
)

set(WEBVIEW2_DLL
    "${WEBVIEW2_ROOT}/x64/WebView2Loader.dll"
)

add_library(NppLatexPreview SHARED
    src/PluginDefinition.cpp
    src/synctex_wrapper.cpp

    third_party/synctex/synctex_parser.c
    third_party/synctex/synctex_parser_utils.c
)

target_include_directories(
    NppLatexPreview
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${WEBVIEW2_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/synctex"
)

target_link_libraries(
    NppLatexPreview
    PRIVATE
        "${WEBVIEW2_LIB}"
        ole32
        user32
        shlwapi
)

target_compile_definitions(
    NppLatexPreview
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# Avoid requiring the MinGW C++ runtime DLLs to be present
# beside Notepad++.
target_link_options(
    NppLatexPreview
    PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic
        -lwinpthread
        -Wl,-Bdynamic
)

set_target_properties(
    NppLatexPreview
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "NppLatexPreview"
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugin"
)

add_custom_command(
    TARGET NppLatexPreview
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND}
        -E copy_if_different
        "${WEBVIEW2_DLL}"
        "$<TARGET_FILE_DIR:NppLatexPreview>/WebView2Loader.dll"
)