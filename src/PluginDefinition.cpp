#include "PluginDefinition.h"

#include <windows.h>
#include <wrl.h>

#include <WebView2.h>
#include <shlwapi.h>

#include <filesystem>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cwctype>

using Microsoft::WRL::ComPtr;


// -----------------------------------------------------------------------------
// Global plugin state
// -----------------------------------------------------------------------------

NppData nppData = {};
FuncItem funcItem[NB_FUNC] = {};

static HINSTANCE g_hInstance = nullptr;

static HWND g_panel = nullptr;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webView;

static bool g_dockingRegistered = false;
static bool g_comInitialized = false;

static bool g_webViewInitializing = false;
static bool g_compileWhenReady = false;

static const wchar_t PANEL_CLASS[] = L"NppLatexPreviewPanel";
static const wchar_t PANEL_NAME[] = L"LaTeX Preview";
static const wchar_t MODULE_NAME[] = L"NppLatexPreview.dll";

// -----------------------------------------------------------------------------
// Header Globals and Helpers
// -----------------------------------------------------------------------------

static bool g_autoCompile = true;
// static bool g_compilePending = false;

constexpr int PREVIEW_HEADER_HEIGHT = 36;

static HWND g_previewHeader = nullptr;
static HWND g_previewStatus = nullptr;

constexpr int IDC_COMPILE_BUTTON = 1001;

static HWND g_compileButton = nullptr;
static bool g_suppressAutoCompile = false;

// namespace NppDarkMode
// {
//     struct Colors
//     {
//         COLORREF background = 0;
//         COLORREF softerBackground = 0;
//         COLORREF hotBackground = 0;
//         COLORREF pureBackground = 0;
//         COLORREF errorBackground = 0;
//         COLORREF text = 0;
//         COLORREF darkerText = 0;
//         COLORREF disabledText = 0;
//         COLORREF linkText = 0;
//         COLORREF edge = 0;
//         COLORREF hotEdge = 0;
//         COLORREF disabledEdge = 0;
//     };
// }

static bool getNppDarkModeColors(
    NppDarkMode::Colors& colors
)
{
    LRESULT result =
        SendMessage(
            nppData._nppHandle,
            NPPM_GETDARKMODECOLORS,
            sizeof(NppDarkMode::Colors),
            reinterpret_cast<LPARAM>(&colors)
        );

    return result != FALSE;
}

static HBRUSH g_previewHeaderBrush = nullptr;
static COLORREF g_previewHeaderColor = GetSysColor(COLOR_WINDOW);
static COLORREF g_previewTextColor = GetSysColor(COLOR_WINDOWTEXT);

static HBRUSH g_compileButtonBrush = nullptr;

static COLORREF g_compileButtonColor = GetSysColor(COLOR_BTNFACE);

static COLORREF g_compileButtonTextColor = GetSysColor(COLOR_BTNTEXT);


static void updatePreviewHeaderTheme()
{
    bool darkMode =
        SendMessage(
            nppData._nppHandle,
            NPPM_ISDARKMODEENABLED,
            0,
            0
        ) != FALSE;

    NppDarkMode::Colors colors;

    if (darkMode)
    {
        BOOL success =
            static_cast<BOOL>(
                SendMessage(
                    nppData._nppHandle,
                    NPPM_GETDARKMODECOLORS,
                    sizeof(colors),
                    reinterpret_cast<LPARAM>(&colors)
                )
            );

        if (success)
        {
            g_previewHeaderColor =
                colors.background;

            g_previewTextColor =
                colors.text;
        }
    }
    else
    {
        g_previewHeaderColor =
            GetSysColor(COLOR_WINDOW);

        g_previewTextColor =
            GetSysColor(COLOR_WINDOWTEXT);
    }

    if (g_previewHeaderBrush)
    {
        DeleteObject(g_previewHeaderBrush);
        g_previewHeaderBrush = nullptr;
    }

    g_previewHeaderBrush =
        CreateSolidBrush(g_previewHeaderColor);

    if (g_previewHeader)
    {
        InvalidateRect(
            g_previewHeader,
            nullptr,
            TRUE
        );
    }

    if (g_previewStatus)
    {
        InvalidateRect(
            g_previewStatus,
            nullptr,
            TRUE
        );
    }

    g_compileButtonColor =
        darkMode
            ? colors.softerBackground
            : GetSysColor(COLOR_BTNFACE);

    g_compileButtonTextColor =
        darkMode
            ? colors.text
            : GetSysColor(COLOR_BTNTEXT);

    if (g_compileButtonBrush)
    {
        DeleteObject(g_compileButtonBrush);
        g_compileButtonBrush = nullptr;
    }

    g_compileButtonBrush =
        CreateSolidBrush(g_compileButtonColor);
}

// -----------------------------------------------------------------------------
// Asynchronous compilation state
// -----------------------------------------------------------------------------

constexpr UINT WM_NPP_LATEX_COMPILE_FINISHED =
    WM_APP + 1;

enum class CompileStatus
{
    Success,
    ProcessStartFailed,
    LatexCompilationFailed,
    PdfMissing,
    UnexpectedError
};

struct CompileResult
{
    CompileStatus status = CompileStatus::ProcessStartFailed;

    std::wstring texPath;
    std::wstring pdfPath;

    int exitCode = -1;

    std::wstring errorMessage;
    std::wstring logText;

    int errorLine = -1;
};

static std::thread g_compileThread;

static std::atomic<bool> g_compileInProgress(false);

static std::mutex g_compileMutex;

static std::unique_ptr<CompileResult> g_pendingCompileResult;

// -----------------------------------------------------------------------------
// WebView2 IIDs
//
// MinGW's __uuidof() handling causes a linker problem with these interfaces,
// so we use the interface IIDs explicitly.
// -----------------------------------------------------------------------------

static const IID IID_WebView2EnvironmentCompletedHandler =
{
    0x4E8A3389,
    0xC9D8,
    0x4BD2,
    { 0xB6, 0xB5, 0x12, 0x4F, 0xEE, 0x6C, 0xC1, 0x4D }
};

static const IID IID_WebView2ControllerCompletedHandler =
{
    0x6C4819F3,
    0xC9B7,
    0x4260,
    { 0x81, 0x27, 0xC9, 0xF5, 0xBD, 0xE7, 0xF6, 0x8C }
};


// -----------------------------------------------------------------------------
// Simple COM callback implementation.
//
// Microsoft::WRL::Callback is unavailable with the MinGW setup being used,
// so we implement the two WebView2 callback interfaces ourselves.
// -----------------------------------------------------------------------------

class EnvironmentCompletedHandler final
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
public:
    using Function =
        std::function<HRESULT(
            HRESULT,
            ICoreWebView2Environment*
        )>;

    explicit EnvironmentCompletedHandler(Function function)
        : m_refCount(1),
          m_function(std::move(function))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** ppvObject
    ) override
    {
        if (ppvObject == nullptr)
            return E_POINTER;

        *ppvObject = nullptr;

        if (riid == IID_IUnknown ||
            riid == IID_WebView2EnvironmentCompletedHandler)
        {
            *ppvObject =
                static_cast<
                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*
                >(this);

            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(
            InterlockedIncrement(&m_refCount)
        );
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = static_cast<ULONG>(
            InterlockedDecrement(&m_refCount)
        );

        if (count == 0)
            delete this;

        return count;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        HRESULT result,
        ICoreWebView2Environment* environment
    ) override
    {
        return m_function(result, environment);
    }

private:
    LONG m_refCount;
    Function m_function;
};


class ControllerCompletedHandler final
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
public:
    using Function =
        std::function<HRESULT(
            HRESULT,
            ICoreWebView2Controller*
        )>;

    explicit ControllerCompletedHandler(Function function)
        : m_refCount(1),
          m_function(std::move(function))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** ppvObject
    ) override
    {
        if (ppvObject == nullptr)
            return E_POINTER;

        *ppvObject = nullptr;

        if (riid == IID_IUnknown ||
            riid == IID_WebView2ControllerCompletedHandler)
        {
            *ppvObject =
                static_cast<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*
                >(this);

            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(
            InterlockedIncrement(&m_refCount)
        );
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = static_cast<ULONG>(
            InterlockedDecrement(&m_refCount)
        );

        if (count == 0)
            delete this;

        return count;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        HRESULT result,
        ICoreWebView2Controller* controller
    ) override
    {
        return m_function(result, controller);
    }

private:
    LONG m_refCount;
    Function m_function;
};


// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

static LRESULT CALLBACK PanelWndProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
);

static bool registerPanelClass();
static bool createPanel();

static void initializeWebView();
static void resizeWebView();
static void shutdownWebView();

static std::wstring getCurrentFilePath();
static bool saveCurrentFile();

static bool runLatexPass(
    const std::wstring& commandLine,
    const std::wstring& workingDirectory,
    DWORD& processError
);

static CompileStatus compileLatex(
    const std::wstring& texPath,
    int& exitCode
);

static std::wstring pathToFileUrl(
    const std::wstring& path
);

static bool compileAndShowPreview();

static void stopCompileThread();

static void setPreviewStatus(
    const wchar_t* status
);

static void updateCompileButton();

// -----------------------------------------------------------------------------
// DLL entry point
// -----------------------------------------------------------------------------

BOOL WINAPI DllMain(
    HINSTANCE hInstance,
    DWORD reason,
    LPVOID
)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }

    return TRUE;
}


// -----------------------------------------------------------------------------
// Helper: show an HRESULT
// -----------------------------------------------------------------------------

static void showHresultError(
    const wchar_t* title,
    const wchar_t* prefix,
    HRESULT hr
)
{
    wchar_t message[512] = {};

    swprintf_s(
        message,
        L"%ls\n\nHRESULT: 0x%08lX",
        prefix,
        static_cast<unsigned long>(hr)
    );

    MessageBoxW(
        nppData._nppHandle,
        message,
        title,
        MB_OK | MB_ICONERROR
    );
}


// -----------------------------------------------------------------------------
// Plugin initialization / cleanup
// -----------------------------------------------------------------------------

void pluginInit()
{
    registerPanelClass();
}


void pluginCleanup()
{
    stopCompileThread();
    g_compileWhenReady = false;
    g_webViewInitializing = false;

    if (g_previewHeaderBrush)
    {
        DeleteObject(g_previewHeaderBrush);
        g_previewHeaderBrush = nullptr;
    }

    if (g_compileButtonBrush)
    {
        DeleteObject(g_compileButtonBrush);
        g_compileButtonBrush = nullptr;
    }

    if (g_panel != nullptr && IsWindow(g_panel))
    {
        SendMessage(
            nppData._nppHandle,
            NPPM_DMMHIDE,
            0,
            reinterpret_cast<LPARAM>(g_panel)
        );

        DestroyWindow(g_panel);
        g_panel = nullptr;
    }

    shutdownWebView();

    if (g_comInitialized)
    {
        CoUninitialize();
        g_comInitialized = false;
    }
}


// -----------------------------------------------------------------------------
// Panel window class
// -----------------------------------------------------------------------------

static bool registerPanelClass()
{
    WNDCLASSW wc = {};

    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = PANEL_CLASS;

    ATOM atom = RegisterClassW(&wc);

    if (atom != 0)
        return true;

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}


// -----------------------------------------------------------------------------
// Create the dockable panel
// -----------------------------------------------------------------------------

static bool createPanel()
{
    if (g_panel != nullptr && IsWindow(g_panel))
        return true;

    if (!registerPanelClass())
        return false;

    g_panel = CreateWindowExW(
        0,
        PANEL_CLASS,
        PANEL_NAME,
        WS_CHILD |
        WS_CLIPCHILDREN |
        WS_CLIPSIBLINGS,
        0,
        0,
        600,
        600,
        nppData._nppHandle,
        nullptr,
        g_hInstance,
        nullptr
    );

    if (g_panel == nullptr)
        return false;

    g_previewHeader =
        CreateWindowExW(
            0,
            L"STATIC",
            nullptr,
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            600,
            PREVIEW_HEADER_HEIGHT,
            g_panel,
            nullptr,
            g_hInstance,
            nullptr
        );

    if (g_previewHeader == nullptr)
    {
        DestroyWindow(g_panel);
        g_panel = nullptr;
        return false;
    }

    g_previewStatus =
        CreateWindowExW(
            0,
            L"STATIC",
            L"Ready",
            WS_CHILD |
            WS_VISIBLE |
            SS_LEFT |
            SS_CENTERIMAGE,
            12,
            0,
            400,
            PREVIEW_HEADER_HEIGHT,
            g_panel,
            nullptr,
            g_hInstance,
            nullptr
        );

    if (g_previewStatus == nullptr)
    {
        DestroyWindow(g_panel);
        g_panel = nullptr;
        g_previewHeader = nullptr;
        return false;
    }

    g_compileButton =
        CreateWindowExW(
            0,
            L"BUTTON",
            L"Compile",
            WS_CHILD |
            WS_VISIBLE |
            BS_OWNERDRAW,
            0,
            0,
            90,
            PREVIEW_HEADER_HEIGHT - 8,
            g_panel,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_COMPILE_BUTTON)
            ),
            g_hInstance,
            nullptr
        );

    if (g_compileButton == nullptr)
    {
        DestroyWindow(g_panel);
        g_panel = nullptr;
        g_previewHeader = nullptr;
        g_previewStatus = nullptr;
        return false;
    }

    HFONT font =
        static_cast<HFONT>(
            GetStockObject(DEFAULT_GUI_FONT)
        );

    SendMessage(
        g_previewStatus,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    updatePreviewHeaderTheme();

    // -------------------------------------------------------------------------
    // Register with Notepad++ docking manager
    // -------------------------------------------------------------------------

    DockedWidgetData dockData = {};

    dockData.hClient = g_panel;
    dockData.pszName = PANEL_NAME;

    // Corresponds to funcItem[0].
    dockData.dlgID = 0;

    // Start on the right side.
    dockData.uMask = DWS_DF_CONT_RIGHT;

    // Actual plugin DLL filename.
    dockData.pszModuleName = MODULE_NAME;

    LRESULT registered = SendMessage(
        nppData._nppHandle,
        NPPM_DMMREGASDCKDLG,
        0,
        reinterpret_cast<LPARAM>(&dockData)
    );

    if (!registered)
    {
        DestroyWindow(g_panel);
        g_panel = nullptr;
        return false;
    }

    g_dockingRegistered = true;

    return true;
}


// -----------------------------------------------------------------------------
// Get the currently active file path
// -----------------------------------------------------------------------------

static std::wstring getCurrentFilePath()
{
    wchar_t buffer[4096] = {};

    LRESULT result = SendMessage(
        nppData._nppHandle,
        NPPM_GETFULLCURRENTPATH,
        static_cast<WPARAM>(
            sizeof(buffer) / sizeof(buffer[0])
        ),
        reinterpret_cast<LPARAM>(buffer)
    );

    if (!result)
        return {};

    return buffer;
}

static std::wstring getFilePathFromBufferId(
    uptr_t bufferId
)
{
    if (bufferId == 0)
        return {};

    int length = static_cast<int>(
        SendMessage(
            nppData._nppHandle,
            NPPM_GETFULLPATHFROMBUFFERID,
            bufferId,
            0
        )
    );

    if (length <= 0)
        return {};

    std::wstring path(
        static_cast<size_t>(length),
        L'\0'
    );

    SendMessage(
        nppData._nppHandle,
        NPPM_GETFULLPATHFROMBUFFERID,
        bufferId,
        reinterpret_cast<LPARAM>(
            path.data()
        )
    );

    return path;
}


// -----------------------------------------------------------------------------
// Notepad++ document helpers
// -----------------------------------------------------------------------------

static bool saveCurrentFile()
{
    SendMessage(
        nppData._nppHandle,
        NPPM_SAVECURRENTFILE,
        0,
        0
    );

    return true;
}

static bool isTexFile(const std::wstring& path)
{
    if (path.size() < 4)
        return false;

    std::wstring extension =
        path.substr(path.size() - 4);

    for (wchar_t& c : extension)
    {
        c = static_cast<wchar_t>(towlower(c));
    }

    return extension == L".tex";
}

// -----------------------------------------------------------------------------
// Execute one pdflatex pass
//
// This is intentionally synchronous for the first working version.
// Later we can move this to a worker thread so Notepad++ stays responsive.
// -----------------------------------------------------------------------------

static bool runLatexPass(
    const std::wstring& commandLine,
    const std::wstring& workingDirectory,
    DWORD& processError
)
{
    processError = ERROR_SUCCESS;

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo = {};

    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end()
    );

    mutableCommand.push_back(L'\0');


    BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &processInfo
    );

    if (!created)
    {
        processError = GetLastError();
        return false;
    }


    WaitForSingleObject(
        processInfo.hProcess,
        INFINITE
    );


    DWORD exitCode = 1;

    if (!GetExitCodeProcess(
        processInfo.hProcess,
        &exitCode
    ))
    {
        processError = GetLastError();

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        return false;
    }


    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (exitCode != 0)
    {
        processError = exitCode;
        return false;
    }

    return true;
}


// -----------------------------------------------------------------------------
// Compile the .tex file
//
// This mirrors the workflow you already use in NppExec:
//
//     pdflatex --shell-escape ...
//     pdflatex ...
//
// The working directory is the directory containing the .tex file.
// -----------------------------------------------------------------------------

enum class ProcessStatus
{
    Started,
    FailedToStart,
    FailedToGetExitCode
};

static ProcessStatus runProcess(
    const std::wstring& commandLine,
    const std::wstring& workingDirectory,
    int& exitCode
)
{
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo = {};

    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(),
        commandLine.end()
    );

    mutableCommandLine.push_back(L'\0');

    BOOL created = CreateProcessW(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &processInfo
    );

    if (!created)
    {
        exitCode = -1;
        return ProcessStatus::FailedToStart;
    }

    WaitForSingleObject(
        processInfo.hProcess,
        INFINITE
    );

    DWORD processExitCode = 0;

    BOOL gotExitCode =
        GetExitCodeProcess(
            processInfo.hProcess,
            &processExitCode
        );

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (!gotExitCode)
    {
        exitCode = -1;
        return ProcessStatus::FailedToGetExitCode;
    }

    exitCode =
        static_cast<int>(processExitCode);

    return ProcessStatus::Started;
}

static CompileStatus compileLatex(
    const std::wstring& texPath,
    int& exitCode
)
{
    std::filesystem::path texFile(texPath);

    std::wstring workingDirectory =
        texFile.parent_path().wstring();

    std::wstring filename =
        texFile.filename().wstring();

    int firstExitCode = -1;
    int secondExitCode = -1;

    std::wstring firstCommand =
        L"pdflatex.exe "
        L"--shell-escape "
        L"--interaction=nonstopmode "
        L"--halt-on-error "
        L"--file-line-error "
        L"\"" + filename + L"\"";

    ProcessStatus firstStatus =
        runProcess(
            firstCommand,
            workingDirectory,
            firstExitCode
        );

    if (firstStatus != ProcessStatus::Started)
    {
        exitCode = -1;
        return CompileStatus::ProcessStartFailed;
    }

    if (firstExitCode != 0)
    {
        exitCode = firstExitCode;
        return CompileStatus::LatexCompilationFailed;
    }

    std::wstring secondCommand =
        L"pdflatex.exe "
        L"--interaction=nonstopmode "
        L"--halt-on-error "
        L"--file-line-error "
        L"\"" + filename + L"\"";

    ProcessStatus secondStatus =
        runProcess(
            secondCommand,
            workingDirectory,
            secondExitCode
        );

    if (secondStatus != ProcessStatus::Started)
    {
        exitCode = -1;
        return CompileStatus::ProcessStartFailed;
    }

    if (secondExitCode != 0)
    {
        exitCode = secondExitCode;
        return CompileStatus::LatexCompilationFailed;
    }

    exitCode = 0;

    return CompileStatus::Success;
}

// -----------------------------------------------------------------------------
// Error Helpers
// -----------------------------------------------------------------------------

static std::string readTextFile(
    const std::filesystem::path& path
)
{
    std::ifstream file(
        path,
        std::ios::binary
    );

    if (!file)
    {
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    return buffer.str();
}

static void parseLatexError(
    const std::string& log,
    std::wstring& errorMessage,
    int& errorLine
)
{
    errorMessage.clear();
    errorLine = -1;

    std::istringstream stream(log);
    std::string line;

    while (std::getline(stream, line))
    {
        // Windows text files may leave a '\r' at the end.
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // ---------------------------------------------------------
        // Parse standard filename.tex:lineno: error
        // ---------------------------------------------------------

        size_t firstColon = line.find(':');

        if (firstColon != std::string::npos)
        {
            size_t numberStart = firstColon + 1;

            size_t numberEnd = numberStart;

            while (
                numberEnd < line.size() &&
                std::isdigit(
                    static_cast<unsigned char>(
                        line[numberEnd]
                    )
                )
            )
            {
                ++numberEnd;
            }

            if (
                numberEnd > numberStart &&
                numberEnd < line.size() &&
                line[numberEnd] == ':'
            )
            {
                try
                {
                    int lineNumber =
                        std::stoi(
                            line.substr(
                                numberStart,
                                numberEnd - numberStart
                            )
                        );

                    std::string message =
                        line.substr(numberEnd + 1);

                    // Remove leading spaces.
                    size_t first =
                        message.find_first_not_of(" \t");

                    if (first != std::string::npos)
                    {
                        message =
                            message.substr(first);
                    }

                    if (!message.empty())
                    {
                        errorLine = lineNumber;

                        errorMessage =
                            std::wstring(
                                message.begin(),
                                message.end()
                            );

                        break;
                    }
                }
                catch (...)
                {
                    // Ignore malformed diagnostics.
                }
            }
        }

        // ---------------------------------------------------------
        // ! Undefined control sequence.
        // ...
        // l.27 ...
        // ---------------------------------------------------------

        if (
            errorMessage.empty() &&
            line.rfind("! ", 0) == 0
        )
        {
            std::string message =
                line.substr(2);

            errorMessage =
                std::wstring(
                    message.begin(),
                    message.end()
                );
        }

        if (
            line.rfind("l.", 0) == 0 &&
            errorLine == -1
        )
        {
            size_t pos = 2;

            while (
                pos < line.size() &&
                std::isdigit(
                    static_cast<unsigned char>(
                        line[pos]
                    )
                )
            )
            {
                ++pos;
            }

            if (pos > 2)
            {
                try
                {
                    errorLine =
                        std::stoi(
                            line.substr(
                                2,
                                pos - 2
                            )
                        );
                }
                catch (...)
                {
                    errorLine = -1;
                }
            }
        }

        // We have everything we need.
        if (
            !errorMessage.empty() &&
            errorLine != -1
        )
        {
            break;
        }
    }

    if (errorMessage.empty())
    {
        errorMessage =
            L"No specific LaTeX error could be identified.";
    }
}

static void jumpToLine(int line)
{
    if (line < 1)
        return;

    HWND scintilla = nppData._scintillaMainHandle;

    if (!scintilla)
        return;

    // LaTeX is 1-indexed; Scintilla is 0-indexed.
    int scintillaLine = line - 1;

    LRESULT lineCount =
        SendMessage(
            scintilla,
            SCI_GETLINECOUNT,
            0,
            0
        );

    if (scintillaLine >= lineCount)
        scintillaLine = static_cast<int>(lineCount) - 1;

    if (scintillaLine < 0)
        return;

    SendMessage(
        scintilla,
        SCI_GOTOLINE,
        scintillaLine,
        0
    );

    SendMessage(
        scintilla,
        SCI_ENSUREVISIBLE,
        scintillaLine,
        0
    );
}

// -----------------------------------------------------------------------------
// Background LaTeX compilation
// -----------------------------------------------------------------------------

static void compileWorker(
    std::wstring texPath
)
{
    auto result =
        std::make_unique<CompileResult>();

    result->texPath = texPath;

    std::filesystem::path pdfPath(texPath);
    pdfPath.replace_extension(L".pdf");

    result->pdfPath =
        pdfPath.wstring();

    try
    {
        // ---------------------------------------------------------
        // TEST ONLY:
        // Deliberately delay compilation so that we can verify
        // that Notepad++ remains responsive (async compilation)
        // ---------------------------------------------------------

        // Sleep(5000);

        result->status =
            compileLatex(
                texPath,
                result->exitCode
            );

        if (result->status ==
            CompileStatus::LatexCompilationFailed)
        {
            std::filesystem::path logPath(texPath);
            logPath.replace_extension(L".log");

            std::string log =
                readTextFile(logPath);

            parseLatexError(
                log,
                result->errorMessage,
                result->errorLine
            );

            result->logText =
                std::wstring(
                    log.begin(),
                    log.end()
                );
        }
        else if (
            result->status ==
            CompileStatus::Success
        )
        {
            // TEST ONLY ↓↓↓ (tests for PdfMissing error)
            //if (std::filesystem::exists(pdfPath))
            //{
            //    std::filesystem::remove(pdfPath);
            //}
            // TEST ONLY ↑↑↑

            if (!std::filesystem::exists(pdfPath))
            {
                result->status =
                    CompileStatus::PdfMissing;

                result->errorMessage =
                    L"pdflatex completed successfully, "
                    L"but the expected PDF could not be found.";
            }
        }
        else if (
            result->status ==
            CompileStatus::ProcessStartFailed
        )
        {
            result->errorMessage =
                L"Could not start pdflatex.exe.\n\n"
                L"Make sure pdflatex is installed and available "
                L"on the system PATH.";
        }
    }
    catch (...)
    {
        result->status = CompileStatus::UnexpectedError;

        result->exitCode = -1;

        result->errorMessage =
            L"An unexpected error occurred while compiling LaTeX.";
    }

    // -------------------------------------------------------------------------
    // Pass the result back to the UI thread.
    // -------------------------------------------------------------------------

    {
        std::lock_guard<std::mutex> lock(
            g_compileMutex
        );

        g_pendingCompileResult =
            std::move(result);
    }

    // -------------------------------------------------------------------------
    // Tell the panel window that compilation has finished.
    //
    // IMPORTANT:
    // We do NOT call WebView2 from this worker thread.
    // -------------------------------------------------------------------------

    if (g_panel != nullptr &&
        IsWindow(g_panel))
    {
        PostMessageW(
            g_panel,
            WM_NPP_LATEX_COMPILE_FINISHED,
            0,
            0
        );
    }
    else
    {
        // No UI window exists anymore.
        //
        // The result will be cleaned up by stopCompileThread().
    }
}

// -----------------------------------------------------------------------------
// Stop the compiler thread safely
// -----------------------------------------------------------------------------

static void stopCompileThread()
{
    if (g_compileThread.joinable())
    {
        g_compileThread.join();
    }

    g_compileInProgress = false;
    updateCompileButton();

    std::lock_guard<std::mutex> lock(
        g_compileMutex
    );

    g_pendingCompileResult.reset();
}

// -----------------------------------------------------------------------------
// Convert a Windows path to a file:// URL
// -----------------------------------------------------------------------------

static std::wstring pathToFileUrl(
    const std::wstring& path
)
{
    // Large enough for normal Windows paths plus URL escaping.
    std::vector<wchar_t> buffer(32768);

    DWORD length =
        static_cast<DWORD>(buffer.size());

    HRESULT hr = UrlCreateFromPathW(
        path.c_str(),
        buffer.data(),
        &length,
        0
    );

    if (FAILED(hr))
        return {};

    return buffer.data();
}


// -----------------------------------------------------------------------------
// Start asynchronous compilation
// -----------------------------------------------------------------------------

static bool compileAndShowPreview()
{
    if (!g_webView)
    {
        MessageBoxW(
            nppData._nppHandle,
            L"WebView2 is not ready yet.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    std::wstring texPath =
        getCurrentFilePath();

    if (texPath.empty())
    {
        MessageBoxW(
            nppData._nppHandle,
            L"Could not determine the current file path.\n\n"
            L"Please save the document as a .tex file first.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    if (!isTexFile(texPath))
    {
        setPreviewStatus(
            L"Current file is not a .tex file"
        );

        return false;
    }

    bool wasAlreadyCompiling =
        g_compileInProgress.exchange(true);

    if (wasAlreadyCompiling)
    {
        MessageBoxW(
            nppData._nppHandle,
            L"A LaTeX compilation is already in progress.",
            PLUGIN_NAME,
            MB_OK | MB_ICONINFORMATION
        );

        return false;
    }

    updateCompileButton();
    setPreviewStatus(L"Compiling...");
    // -------------------------------------------------------------------------
    // Save current document.
    // -------------------------------------------------------------------------

    g_suppressAutoCompile = true;

    SendMessage(
        nppData._nppHandle,
        NPPM_SAVECURRENTFILE,
        0,
        0
    );

    g_suppressAutoCompile = false;

    // std::wstring texPath =
    //     getCurrentFilePath();

    if (texPath.empty())
    {
        g_compileInProgress = false;
        updateCompileButton();

        MessageBoxW(
            nppData._nppHandle,
            L"Could not determine the current file path.\n\n"
            L"Please save the document as a .tex file first.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    if (!isTexFile(texPath))
    {
        g_compileInProgress = false;
        updateCompileButton();

        setPreviewStatus(L"Not a .tex file");

        return false;
    }

    // -------------------------------------------------------------------------
    // A previous thread may have finished but not yet been joined.
    //
    // Joining it here is effectively instantaneous in that situation.
    // -------------------------------------------------------------------------

    if (g_compileThread.joinable())
    {
        g_compileThread.join();
    }

    // -------------------------------------------------------------------------
    // Remove any stale result.
    // -------------------------------------------------------------------------

    {
        std::lock_guard<std::mutex> lock(
            g_compileMutex
        );

        g_pendingCompileResult.reset();
    }

    // -------------------------------------------------------------------------
    // Start the worker thread.
    // -------------------------------------------------------------------------

    g_compileThread =
        std::thread(
            compileWorker,
            texPath
        );

    return true;
}


// -----------------------------------------------------------------------------
// Show the preview panel
// -----------------------------------------------------------------------------

void showPreviewPanel()
{
    if (!createPanel())
    {
        MessageBoxW(
            nppData._nppHandle,
            L"Could not create the LaTeX Preview panel.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return;
    }


    // -------------------------------------------------------------------------
    // Initialize COM on the Notepad++ UI thread
    // -------------------------------------------------------------------------

    if (!g_comInitialized)
    {
        HRESULT comResult =
            CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED
            );

        if (SUCCEEDED(comResult))
        {
            g_comInitialized = true;
        }
        else if (comResult == RPC_E_CHANGED_MODE)
        {
            MessageBoxW(
                nppData._nppHandle,
                L"Notepad++ is using an incompatible COM apartment.",
                PLUGIN_NAME,
                MB_OK | MB_ICONERROR
            );

            return;
        }
        else
        {
            showHresultError(
                PLUGIN_NAME,
                L"CoInitializeEx failed.",
                comResult
            );

            return;
        }
    }


    // -------------------------------------------------------------------------
    // Initialize WebView2 if necessary
    // -------------------------------------------------------------------------

    if (!g_webView)
    {
        if (g_webViewInitializing)
        {
            // WebView2 is already being initialized.
            // The first request will be handled once it becomes ready.
            g_compileWhenReady = true;
        }
        else
        {
            g_compileWhenReady = true;
            initializeWebView();
        }
    }
    else
    {
        // WebView2 already exists, so we can compile immediately.
        compileAndShowPreview();
    }


    // -------------------------------------------------------------------------
    // Show the panel
    // -------------------------------------------------------------------------

    SendMessage(
        nppData._nppHandle,
        NPPM_DMMVIEWOTHERTAB,
        0,
        reinterpret_cast<LPARAM>(PANEL_NAME)
    );
}


// -----------------------------------------------------------------------------
// WebView2 initialization
// -----------------------------------------------------------------------------

static void initializeWebView()
{
    if (g_panel == nullptr ||
        !IsWindow(g_panel))
    {
        g_compileWhenReady = false;
        return;
    }


    if (g_webViewInitializing)
        return;


    g_webViewInitializing = true;


    // -------------------------------------------------------------------------
    // Put WebView2 user data somewhere writable.
    //
    // Program Files is not a good location for WebView2 profile data, so use:
    //
    // C:\Users\<user>\AppData\Local\NppLatexPreview\WebView2
    // -------------------------------------------------------------------------

    wchar_t localAppData[MAX_PATH] = {};

    DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData,
        MAX_PATH
    );

    if (length == 0 ||
        length >= MAX_PATH)
    {
        g_webViewInitializing = false;
        g_compileWhenReady = false;

        MessageBoxW(
            nppData._nppHandle,
            L"Could not determine LOCALAPPDATA.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return;
    }


    std::wstring userDataFolder =
        std::wstring(localAppData) +
        L"\\NppLatexPreview\\WebView2";


    // -------------------------------------------------------------------------
    // Environment callback
    // -------------------------------------------------------------------------

    auto* environmentHandler =
        new (std::nothrow) EnvironmentCompletedHandler(
            [](
                HRESULT result,
                ICoreWebView2Environment* environment
            ) -> HRESULT
            {
                if (FAILED(result) ||
                    environment == nullptr)
                {
                    g_webViewInitializing = false;
                    g_compileWhenReady = false;

                    showHresultError(
                        PLUGIN_NAME,
                        L"Failed to create the WebView2 environment.",
                        FAILED(result)
                            ? result
                            : E_FAIL
                    );

                    return FAILED(result)
                        ? result
                        : E_FAIL;
                }


                if (g_panel == nullptr ||
                    !IsWindow(g_panel))
                {
                    g_webViewInitializing = false;
                    g_compileWhenReady = false;
                    return S_OK;
                }


                // -----------------------------------------------------------------
                // Controller callback
                // -----------------------------------------------------------------

                auto* controllerHandler =
                    new (std::nothrow) ControllerCompletedHandler(
                        [](
                            HRESULT result,
                            ICoreWebView2Controller* controller
                        ) -> HRESULT
                        {
                            if (FAILED(result) ||
                                controller == nullptr)
                            {
                                g_webViewInitializing = false;
                                g_compileWhenReady = false;

                                showHresultError(
                                    PLUGIN_NAME,
                                    L"Failed to create the WebView2 controller.",
                                    FAILED(result)
                                        ? result
                                        : E_FAIL
                                );

                                return FAILED(result)
                                    ? result
                                    : E_FAIL;
                            }


                            g_controller = controller;


                            HRESULT hr =
                                controller->get_CoreWebView2(
                                    g_webView.GetAddressOf()
                                );

                            if (FAILED(hr) ||
                                !g_webView)
                            {
                                g_webViewInitializing = false;
                                g_compileWhenReady = false;

                                showHresultError(
                                    PLUGIN_NAME,
                                    L"Could not obtain ICoreWebView2.",
                                    FAILED(hr)
                                        ? hr
                                        : E_FAIL
                                );

                                return FAILED(hr)
                                    ? hr
                                    : E_FAIL;
                            }


                            controller->put_IsVisible(TRUE);

                            resizeWebView();


                            // ---------------------------------------------------------
                            // WebView2 is now fully ready.
                            // ---------------------------------------------------------

                            g_webViewInitializing = false;


                            if (g_compileWhenReady)
                            {
                                g_compileWhenReady = false;

                                // This performs the first complete milestone:
                                //
                                // save .tex
                                // compile twice
                                // navigate to resulting PDF
                                compileAndShowPreview();
                            }


                            return S_OK;
                        }
                    );


                if (controllerHandler == nullptr)
                {
                    g_webViewInitializing = false;
                    g_compileWhenReady = false;

                    return E_OUTOFMEMORY;
                }


                HRESULT controllerResult =
                    environment->CreateCoreWebView2Controller(
                        g_panel,
                        controllerHandler
                    );


                // CreateCoreWebView2Controller takes ownership of the
                // callback reference when appropriate, so release our own
                // reference.
                controllerHandler->Release();


                return controllerResult;
            }
        );


    if (environmentHandler == nullptr)
    {
        g_webViewInitializing = false;
        g_compileWhenReady = false;

        MessageBoxW(
            nppData._nppHandle,
            L"Could not allocate the WebView2 environment callback.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return;
    }


    // -------------------------------------------------------------------------
    // Create WebView2 environment
    // -------------------------------------------------------------------------

    HRESULT hr =
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            userDataFolder.c_str(),
            nullptr,
            environmentHandler
        );


    // We own one reference to the callback.
    environmentHandler->Release();


    if (FAILED(hr))
    {
        g_webViewInitializing = false;
        g_compileWhenReady = false;

        showHresultError(
            PLUGIN_NAME,
            L"CreateCoreWebView2EnvironmentWithOptions failed.",
            hr
        );
    }
}


// -----------------------------------------------------------------------------
// Resize WebView2 whenever the docking panel changes size
// -----------------------------------------------------------------------------

static void resizeWebView()
{
    if (!g_controller ||
        !g_panel ||
        !IsWindow(g_panel))
    {
        return;
    }

    RECT bounds = {};

    GetClientRect(
        g_panel,
        &bounds
    );

    int width =
        bounds.right - bounds.left;

    int height =
        bounds.bottom - bounds.top;

    constexpr int BUTTON_WIDTH = 90;
    constexpr int BUTTON_MARGIN = 8;

    if (g_previewHeader)
    {
        SetWindowPos(
            g_previewHeader,
            nullptr,
            0,
            0,
            width,
            PREVIEW_HEADER_HEIGHT,
            SWP_NOZORDER
        );
    }

    if (g_previewStatus)
    {
        SetWindowPos(
            g_previewStatus,
            nullptr,
            12,
            0,
            width - BUTTON_WIDTH - BUTTON_MARGIN - 24,
            PREVIEW_HEADER_HEIGHT,
            SWP_NOZORDER
        );
    }

    if (g_compileButton)
    {
        SetWindowPos(
            g_compileButton,
            nullptr,
            width - BUTTON_WIDTH - BUTTON_MARGIN,
            4,
            BUTTON_WIDTH,
            PREVIEW_HEADER_HEIGHT - 8,
            SWP_NOZORDER
        );
    }

    RECT webViewBounds =
    {
        0,
        PREVIEW_HEADER_HEIGHT,
        width,
        height
    };

    g_controller->put_Bounds(
        webViewBounds
    );
}

// -----------------------------------------------------------------------------
// Other WebView2 and Header Helpers
// -----------------------------------------------------------------------------

static void setPreviewStatus(
    const wchar_t* status
)
{
    if (g_previewStatus &&
        IsWindow(g_previewStatus))
    {
        SetWindowTextW(
            g_previewStatus,
            status
        );
    }
}

static void updateCompileButton()
{
    if (!g_compileButton ||
        !IsWindow(g_compileButton))
    {
        return;
    }

    EnableWindow(
        g_compileButton,
        g_compileInProgress ? FALSE : TRUE
    );
}

// -----------------------------------------------------------------------------
// Shut WebView2 down
// -----------------------------------------------------------------------------

static void shutdownWebView()
{
    g_webViewInitializing = false;
    g_compileWhenReady = false;

    if (g_controller)
    {
        g_controller->Close();
    }

    g_webView.Reset();
    g_controller.Reset();
}


// -----------------------------------------------------------------------------
// Panel window procedure
// -----------------------------------------------------------------------------

static LRESULT CALLBACK PanelWndProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (message)
    {
        case WM_SIZE:
        {
            resizeWebView();
            return 0;
        }
        
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc =
                reinterpret_cast<HDC>(wParam);

            HWND control =
                reinterpret_cast<HWND>(lParam);

            if (control == g_previewHeader ||
                control == g_previewStatus)
            {
                SetTextColor(
                    hdc,
                    g_previewTextColor
                );

                SetBkColor(
                    hdc,
                    g_previewHeaderColor
                );

                return reinterpret_cast<LRESULT>(
                    g_previewHeaderBrush
                );
            }

            break;
        }

        // case WM_CTLCOLORBTN:
        // {
        //     HDC hdc =
        //         reinterpret_cast<HDC>(wParam);

        //     HWND control =
        //         reinterpret_cast<HWND>(lParam);

        //     if (control == g_compileButton)
        //     {
        //         SetTextColor(
        //             hdc,
        //             g_compileButtonTextColor
        //         );

        //         SetBkColor(
        //             hdc,
        //             g_compileButtonColor
        //         );

        //         return reinterpret_cast<LRESULT>(
        //             g_compileButtonBrush
        //         );
        //     }

        //     break;
        // }

        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* drawItem =
                reinterpret_cast<DRAWITEMSTRUCT*>(lParam);

            if (
                drawItem != nullptr &&
                drawItem->CtlID == IDC_COMPILE_BUTTON
            )
            {
                HDC hdc = drawItem->hDC;

                RECT rect = drawItem->rcItem;

                bool disabled =
                    (drawItem->itemState & ODS_DISABLED) != 0;

                bool pressed =
                    (drawItem->itemState & ODS_SELECTED) != 0;

                COLORREF background =
                    g_compileButtonColor;

                COLORREF textColor =
                    g_compileButtonTextColor;

                if (disabled)
                {
                    textColor =
                        RGB(
                            GetRValue(textColor) / 2,
                            GetGValue(textColor) / 2,
                            GetBValue(textColor) / 2
                        );
                }

                if (pressed)
                {
                    background =
                        g_previewHeaderColor;
                }

                HBRUSH brush =
                    CreateSolidBrush(background);

                FillRect(
                    hdc,
                    &rect,
                    brush
                );

                DeleteObject(brush);

                SetBkMode(
                    hdc,
                    TRANSPARENT
                );

                SetTextColor(
                    hdc,
                    textColor
                );

                DrawTextW(
                    hdc,
                    L"Compile",
                    -1,
                    &rect,
                    DT_CENTER |
                    DT_VCENTER |
                    DT_SINGLELINE
                );

                return TRUE;
            }

            break;
        }

        case WM_COMMAND:
        {
            if (
                LOWORD(wParam) == IDC_COMPILE_BUTTON &&
                HIWORD(wParam) == BN_CLICKED
            )
            {
                compileAndShowPreview();
                return 0;
            }

            break;
        }

        case WM_NPP_LATEX_COMPILE_FINISHED:
        {
            std::unique_ptr<CompileResult> result;

            // -------------------------------------------------------------------------
            // Take ownership of the result produced by the worker thread.
            // -------------------------------------------------------------------------

            {
                std::lock_guard<std::mutex> lock(
                    g_compileMutex
                );

                result =
                    std::move(g_pendingCompileResult);
            }

            if (!result)
            {
                g_compileInProgress = false;
                updateCompileButton();
                return 0;
            }

            // -------------------------------------------------------------------------
            // Compilation is finished from the user's perspective.
            // -------------------------------------------------------------------------

            g_compileInProgress = false;
            updateCompileButton();

            // -------------------------------------------------------------------------
            // The worker has already posted the message, so it should be finishing.
            // Joining here guarantees that the std::thread object is cleaned up
            // before another compilation can start.
            // -------------------------------------------------------------------------

            if (g_compileThread.joinable())
            {
                g_compileThread.join();
            }

            // -------------------------------------------------------------------------
            // Compilation failed.
            //
            // Milestone 7 (done) replaces generic error with useful information
            // parsed from the .log file.
            // -------------------------------------------------------------------------

            if (result->status != CompileStatus::Success)
            {
                switch (result->status)
                {
                    case CompileStatus::ProcessStartFailed:
                        setPreviewStatus(
                            L"Could not start pdflatex"
                        );
                        break;

                    case CompileStatus::LatexCompilationFailed:
                        setPreviewStatus(
                            L"Compilation failed"
                        );
                        break;

                    case CompileStatus::PdfMissing:
                        setPreviewStatus(
                            L"PDF was not generated"
                        );
                        break;

                    case CompileStatus::UnexpectedError:
                        setPreviewStatus(
                            L"Unexpected compilation error"
                        );
                        break;

                    default:
                        setPreviewStatus(
                            L"Compilation failed"
                        );
                        break;
                }

                std::wstring message =
                    result->errorMessage;

                if (
                    result->status ==
                    CompileStatus::LatexCompilationFailed
                )
                {
                    message +=
                        L"\n\nExit code: " +
                        std::to_wstring(result->exitCode);

                    if (result->errorLine != -1)
                    {
                        message +=
                            L"\nLine: " +
                            std::to_wstring(result->errorLine);
                    }
                }

                MessageBoxW(
                    nppData._nppHandle,
                    message.c_str(),
                    L"LaTeX compilation failed",
                    MB_OK | MB_ICONERROR
                );

                if (
                    result->status ==
                        CompileStatus::LatexCompilationFailed &&
                    result->errorLine != -1
                )
                {
                    jumpToLine(result->errorLine);
                }

                return 0;
            }

            // -------------------------------------------------------------------------
            // Convert the generated PDF to a file:// URL.
            // -------------------------------------------------------------------------

            std::wstring pdfUrl =
                pathToFileUrl(
                    result->pdfPath
                );

            if (pdfUrl.empty())
            {
                setPreviewStatus(
                    L"Could not load PDF"
                );

                MessageBoxW(
                    nppData._nppHandle,
                    L"Could not convert the PDF path to a file URL.",
                    PLUGIN_NAME,
                    MB_OK | MB_ICONERROR
                );

                return 0;
            }

            // -------------------------------------------------------------------------
            // IMPORTANT:
            //
            // This is now running on the Notepad++ UI thread, so calling WebView2
            // here is safe.
            // -------------------------------------------------------------------------

            if (!g_webView)
            {
                setPreviewStatus(
                    L"Preview unavailable"
                );

                MessageBoxW(
                    nppData._nppHandle,
                    L"WebView2 is no longer available.",
                    PLUGIN_NAME,
                    MB_OK | MB_ICONERROR
                );

                return 0;
            }

            HRESULT hr =
                g_webView->Navigate(
                    pdfUrl.c_str()
                );

            if (FAILED(hr))
            {
                setPreviewStatus(
                    L"PDF preview failed"
                );

                showHresultError(
                    PLUGIN_NAME,
                    L"WebView2 failed to navigate to the PDF.",
                    hr
                );
            }
            else
            {
                setPreviewStatus(
                    L"Compiled successfully"
                );
            }

            return 0;
        }

        case WM_DESTROY:
        {
            shutdownWebView();

            g_previewStatus = nullptr;
            g_previewHeader = nullptr;

            g_panel = nullptr;
            g_dockingRegistered = false;

            return 0;
        }


        default:
            break;
    }


    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}


// -----------------------------------------------------------------------------
// Notepad++ plugin interface
// -----------------------------------------------------------------------------

extern "C"
__declspec(dllexport)
void setInfo(NppData notepadPlusData)
{
    nppData = notepadPlusData;

    pluginInit();


    // -------------------------------------------------------------------------
    // Menu command
    // -------------------------------------------------------------------------

    lstrcpyW(
        funcItem[0]._itemName,
        L"Show LaTeX Preview"
    );

    funcItem[0]._pFunc = showPreviewPanel;
    funcItem[0]._init2Check = false;
    funcItem[0]._pShKey = nullptr;
}


extern "C"
__declspec(dllexport)
const wchar_t* getName()
{
    return PLUGIN_NAME;
}


extern "C"
__declspec(dllexport)
FuncItem* getFuncsArray(int* nbF)
{
    *nbF = NB_FUNC;
    return funcItem;
}


extern "C"
__declspec(dllexport)
void beNotified(SCNotification* notifyCode)
{
    if (notifyCode == nullptr)
        return;

    switch (notifyCode->nmhdr.code)
    {
        case NPPN_DARKMODECHANGED:
        {
            updatePreviewHeaderTheme();
            break;
        }

        case NPPN_FILESAVED:
        {
            if (!g_autoCompile || g_suppressAutoCompile)
                break;

            std::wstring savedPath =
                getFilePathFromBufferId(
                    notifyCode->nmhdr.idFrom
                );
                

            if (savedPath.empty())
                break;

            if (!isTexFile(savedPath))
                break;

            // Only compile if this is the document
            // currently being previewed/edited.
            std::wstring currentPath =
                getCurrentFilePath();

            if (currentPath.empty() ||
                savedPath != currentPath)
            {
                break;
            }

            compileAndShowPreview();

            break;
        }

        default:
            break;
    }
}


extern "C"
__declspec(dllexport)
LRESULT messageProc(
    UINT,
    WPARAM,
    LPARAM
)
{
    return FALSE;
}


extern "C"
__declspec(dllexport)
BOOL isUnicode()
{
    return TRUE;
}