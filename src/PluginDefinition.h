#pragma once

#include <windows.h>

#include "PluginInterface.h"
#include "Docking.h"

#define PLUGIN_NAME L"NppLatexPreview"
#define NB_FUNC 1

extern NppData nppData;
extern FuncItem funcItem[NB_FUNC];

void showPreviewPanel();

void pluginInit();
void pluginCleanup();