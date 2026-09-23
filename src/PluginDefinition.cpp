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

static bool compileLatex(
    const std::wstring& texPath
);

static std::wstring pathToFileUrl(
    const std::wstring& path
);

static bool compileAndShowPreview();


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
    g_compileWhenReady = false;
    g_webViewInitializing = false;

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


// -----------------------------------------------------------------------------
// Save current Notepad++ document
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

static bool compileLatex(
    const std::wstring& texPath
)
{
    namespace fs = std::filesystem;

    fs::path texFile(texPath);


    if (!fs::exists(texFile))
    {
        MessageBoxW(
            nppData._nppHandle,
            L"The current .tex file does not exist.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }


    if (_wcsicmp(
        texFile.extension().c_str(),
        L".tex"
    ) != 0)
    {
        MessageBoxW(
            nppData._nppHandle,
            L"The current file is not a .tex file.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }


    fs::path workingDirectory =
        texFile.parent_path();

    std::wstring filename =
        texFile.filename().wstring();


    // -------------------------------------------------------------------------
    // First pass
    // -------------------------------------------------------------------------

    std::wstring firstPass =
        L"pdflatex.exe "
        L"--shell-escape "
        L"--interaction=nonstopmode "
        L"--halt-on-error "
        L"--file-line-error "
        L"\"" + filename + L"\"";


    DWORD processError = ERROR_SUCCESS;

    if (!runLatexPass(
        firstPass,
        workingDirectory.wstring(),
        processError
    ))
    {
        wchar_t message[512] = {};

        swprintf_s(
            message,
            L"First pdflatex pass failed.\n\n"
            L"Error code: %lu\n\n"
            L"See the generated .log file for details.",
            static_cast<unsigned long>(processError)
        );

        MessageBoxW(
            nppData._nppHandle,
            message,
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }


    // -------------------------------------------------------------------------
    // Second pass
    // -------------------------------------------------------------------------

    std::wstring secondPass =
        L"pdflatex.exe "
        L"--interaction=nonstopmode "
        L"--halt-on-error "
        L"--file-line-error "
        L"\"" + filename + L"\"";


    processError = ERROR_SUCCESS;

    if (!runLatexPass(
        secondPass,
        workingDirectory.wstring(),
        processError
    ))
    {
        wchar_t message[512] = {};

        swprintf_s(
            message,
            L"Second pdflatex pass failed.\n\n"
            L"Error code: %lu\n\n"
            L"See the generated .log file for details.",
            static_cast<unsigned long>(processError)
        );

        MessageBoxW(
            nppData._nppHandle,
            message,
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }


    // -------------------------------------------------------------------------
    // Check resulting PDF
    // -------------------------------------------------------------------------

    fs::path pdfPath = texFile;
    pdfPath.replace_extension(L".pdf");

    if (!fs::exists(pdfPath))
    {
        MessageBoxW(
            nppData._nppHandle,
            L"pdflatex completed, but the expected PDF was not created.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    return true;
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
// Compile current .tex and display the resulting PDF
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

    // Ask Notepad++ to save the current document.
    SendMessage(
        nppData._nppHandle,
        NPPM_SAVECURRENTFILE,
        0,
        0
    );

    // Get the path after saving.
    std::wstring texPath = getCurrentFilePath();

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

    // Compile.
    if (!compileLatex(texPath))
        return false;

    // Find resulting PDF.
    std::filesystem::path pdfPath(texPath);
    pdfPath.replace_extension(L".pdf");

    if (!std::filesystem::exists(pdfPath))
    {
        MessageBoxW(
            nppData._nppHandle,
            L"The compiled PDF could not be found.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    // Convert PDF path to file:// URL.
    std::wstring pdfUrl =
        pathToFileUrl(pdfPath.wstring());

    if (pdfUrl.empty())
    {
        MessageBoxW(
            nppData._nppHandle,
            L"Could not convert the PDF path to a file URL.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    HRESULT hr =
        g_webView->Navigate(pdfUrl.c_str());

    if (FAILED(hr))
    {
        showHresultError(
            PLUGIN_NAME,
            L"WebView2 failed to navigate to the PDF.",
            hr
        );

        return false;
    }

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


    g_controller->put_Bounds(bounds);
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


        case WM_DESTROY:
        {
            shutdownWebView();

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
void beNotified(SCNotification* notification)
{
    if (notification == nullptr)
        return;


    // Clean up when Notepad++ shuts down.
    if (notification->nmhdr.code == NPPN_SHUTDOWN)
    {
        pluginCleanup();
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