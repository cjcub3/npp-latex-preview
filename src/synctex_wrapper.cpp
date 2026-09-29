#include "synctex_wrapper.h"

#include "synctex_parser.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

static std::string wideToUtf8(
    const std::wstring& value
)
{
    if (value.empty())
        return {};

    int size =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (size <= 0)
        return {};

    std::string result(
        static_cast<size_t>(size),
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );

    return result;
}

static std::wstring utf8ToWide(
    const char* value
)
{
    if (value == nullptr || *value == '\0')
        return {};

    int size =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            value,
            -1,
            nullptr,
            0
        );

    if (size <= 1)
        return {};

    std::wstring result(
        static_cast<size_t>(size - 1),
        L'\0'
    );

    MultiByteToWideChar(
        CP_UTF8,
        0,
        value,
        -1,
        result.data(),
        size
    );

    return result;
}

bool syncTeXForwardSearch(
    const std::wstring& pdfPath,
    const std::wstring& texPath,
    int line,
    int column,
    SyncTeXLocation& location
)
{
    location = {};

    if (line < 1)
        return false;

    std::string pdfUtf8 =
        wideToUtf8(pdfPath);

    std::string texUtf8 =
        wideToUtf8(texPath);

    if (
        pdfUtf8.empty() ||
        texUtf8.empty()
    )
    {
        return false;
    }

    synctex_scanner_p scanner =
        synctex_scanner_new_with_output_file(
            pdfUtf8.c_str(),
            nullptr,
            1
        );

    if (!scanner)
        return false;

    synctex_status_t queryResult =
        synctex_display_query(
            scanner,
            texUtf8.c_str(),
            line,
            column,
            0
        );

    if (queryResult <= 0)
    {
        synctex_scanner_free(scanner);
        return false;
    }

    synctex_node_p node =
        synctex_scanner_next_result(
            scanner
        );

    if (!node)
    {
        synctex_scanner_free(scanner);
        return false;
    }

    location.page =
        synctex_node_page(node);

    location.x =
        synctex_node_box_visible_h(node);

    location.y =
        synctex_node_box_visible_v(node);

    location.sourceFile =
        utf8ToWide(
            synctex_node_get_name(node)
        );

    location.line =
        synctex_node_line(node);

    location.column =
        synctex_node_column(node);

    synctex_scanner_free(scanner);

    return location.page > 0;
}

bool syncTeXInverseSearch(
    const std::wstring& pdfPath,
    int page,
    float h,
    float v,
    SyncTeXLocation& location
)
{
    location = {};

    if (page < 1)
        return false;

    std::string pdfUtf8 = wideToUtf8(pdfPath);

    if (pdfUtf8.empty())
        return false;

    synctex_scanner_p scanner =
        synctex_scanner_new_with_output_file(
            pdfUtf8.c_str(),
            nullptr,
            1
        );

    if (!scanner)
        return false;

    /*
        synctex_edit_query() does NOT return a node.

        It returns the number of matching nodes (or a
        negative error status). The nodes themselves are
        retrieved with synctex_scanner_next_result().
    */
    synctex_status_t queryResult =
        synctex_edit_query(scanner, page, h, v);

    if (queryResult <= 0)
    {
        synctex_scanner_free(scanner);
        return false;
    }

    synctex_node_p node =
        synctex_scanner_next_result(scanner);

    if (!node)
    {
        synctex_scanner_free(scanner);
        return false;
    }

    location.page = synctex_node_page(node);

    location.x = synctex_node_visible_h(node);
    location.y = synctex_node_visible_v(node);

    location.sourceFile =
        utf8ToWide(synctex_node_get_name(node));

    location.line = synctex_node_line(node);
    location.column = synctex_node_column(node);

    synctex_scanner_free(scanner);

    return location.page > 0 &&
           !location.sourceFile.empty() &&
           location.line > 0;
}