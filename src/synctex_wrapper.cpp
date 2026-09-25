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

bool syncTeXForwardSearch(
    const std::wstring& synctexPath,
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

    synctex_scanner_free(scanner);

    return location.page > 0;
}