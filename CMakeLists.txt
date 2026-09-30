cmake_minimum_required(VERSION 3.20)

project(NppLatexPreview LANGUAGES C CXX)

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

# if(WIN32 AND MINGW)
#     # Forces static linking of all runtime libraries globally
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
#     set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")
# endif()

add_library(NppLatexPreview SHARED
    src/PluginDefinition.cpp
    src/synctex_wrapper.cpp

    third_party/synctex/synctex_parser.c
    third_party/synctex/synctex_parser_utils.c

    # zlib
    third_party/zlib/adler32.c
    third_party/zlib/compress.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/gzclose.c
    third_party/zlib/gzlib.c
    third_party/zlib/gzread.c
    third_party/zlib/gzwrite.c
    third_party/zlib/infback.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/trees.c
    third_party/zlib/uncompr.c
    third_party/zlib/zutil.c
)

target_include_directories(
    NppLatexPreview
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${WEBVIEW2_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/synctex"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib"
)

# set(WINPTHREAD_STATIC
#     "C:/mingw64/x86_64-w64-mingw32/lib/libwinpthread.a"
# )

#

target_link_libraries(
    NppLatexPreview
    PRIVATE
        "${WEBVIEW2_LIB}"
        #"${WINPTHREAD_STATIC}"
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

target_compile_options(
    NppLatexPreview
    PRIVATE
        -pthread
)

target_link_options(
    NppLatexPreview
    PRIVATE
        -static-libgcc
        -static-libstdc++
        #-no-pthread
        #-Wl,-Bstatic
        #-lpthread
        #-lwinpthread
        #-Wl,-Bdynamic
        #-Wl,-t
        -static
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