#pragma once

#include <string>

struct SyncTeXLocation
{
    std::wstring sourceFile;

    int line = -1;
    int column = -1;

    int page = -1;

    double x = 0.0;
    double y = 0.0;
};

bool syncTeXForwardSearch(
    const std::wstring& pdfPath,
    const std::wstring& texPath,
    int line,
    int column,
    SyncTeXLocation& location
);