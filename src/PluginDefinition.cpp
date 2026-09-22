#include "PluginDefinition.h"

#include <windows.h>
#include <wrl.h>
#include <string>
#include <functional>
#include <new>

#include <WebView2.h>

using Microsoft::WRL::ComPtr;

// WebView2 callback interface IIDs.
// Using explicit IIDs avoids MinGW's unresolved __mingw_uuidof<> symbols.

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

static const wchar_t PANEL_CLASS[] = L"NppLatexPreviewPanel";
static const wchar_t PANEL_NAME[] = L"LaTeX Preview";
static const wchar_t MODULE_NAME[] = L"NppLatexPreview.dll";


// -----------------------------------------------------------------------------
// MinGW-compatible WebView2 callback helpers
//
// Microsoft's usual WebView2 examples use WRL::Callback<> here.
// The MinGW environment being used for this project does not provide
// that helper, so we implement the two required COM callback interfaces
// directly.
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
                static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(
                    this
                );

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
                static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(
                    this
                );

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
// Plugin initialization / cleanup
// -----------------------------------------------------------------------------

void pluginInit()
{
    registerPanelClass();
}


void pluginCleanup()
{
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

    // Notepad++'s docking manager uses this information to turn
    // our window into a dockable panel.
    DockedWidgetData dockData;

    dockData.hClient = g_panel;
    dockData.pszName = PANEL_NAME;

    // This corresponds to funcItem[0], which is showPreviewPanel().
    dockData.dlgID = 0;

    // Start docked on the right side.
    dockData.uMask = DWS_DF_CONT_RIGHT;

    // This MUST be the actual plugin DLL filename.
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
// Show panel
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

    // Initialize COM on the Notepad++ UI thread.
    if (!g_comInitialized)
    {
        HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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
            MessageBoxW(
                nppData._nppHandle,
                L"CoInitializeEx failed.",
                PLUGIN_NAME,
                MB_OK | MB_ICONERROR
            );

            return;
        }
    }

    if (!g_webView)
    {
        initializeWebView();
    }

    // Switch the docking manager to our panel.
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
    if (g_panel == nullptr)
        return;
	
	HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
	{
		wchar_t message[256];
		swprintf_s(
			message,
			L"CoInitializeEx failed: 0x%08lX",
			static_cast<unsigned long>(comHr)
		);

		MessageBox(
			g_panel,
			message,
			L"NppLatexPreview",
			MB_OK | MB_ICONERROR
		);

		return;
	}

    auto* environmentHandler =
        new (std::nothrow) EnvironmentCompletedHandler(
            [](
                HRESULT result,
                ICoreWebView2Environment* environment
            ) -> HRESULT
            {
                if (FAILED(result) || environment == nullptr)
                {
                    MessageBoxW(
                        nppData._nppHandle,
                        L"Failed to create the WebView2 environment.",
                        PLUGIN_NAME,
                        MB_OK | MB_ICONERROR
                    );

                    return FAILED(result) ? result : E_FAIL;
                }

                if (g_panel == nullptr || !IsWindow(g_panel))
                    return S_OK;

                auto* controllerHandler =
                    new (std::nothrow) ControllerCompletedHandler(
                        [](
                            HRESULT result,
                            ICoreWebView2Controller* controller
                        ) -> HRESULT
                        {
                            if (FAILED(result) || controller == nullptr)
                            {
                                MessageBoxW(
                                    nppData._nppHandle,
                                    L"Failed to create the WebView2 controller.",
                                    PLUGIN_NAME,
                                    MB_OK | MB_ICONERROR
                                );

                                return FAILED(result) ? result : E_FAIL;
                            }

                            g_controller = controller;

                            HRESULT hr =
                                controller->get_CoreWebView2(
                                    g_webView.GetAddressOf()
                                );

                            if (FAILED(hr) || !g_webView)
                                return FAILED(hr) ? hr : E_FAIL;

                            controller->put_IsVisible(TRUE);

                            resizeWebView();

                            // For now, just display a test page.
                            // Later this will become the local PDF viewer.
                            const wchar_t* testHtml =
                                LR"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>NppLatexPreview</title>
<style>
    html, body {
        height: 100%;
        margin: 0;
        font-family: Arial, sans-serif;
        background: #202020;
        color: #eeeeee;
    }

    body {
        display: flex;
        align-items: center;
        justify-content: center;
        flex-direction: column;
    }

    h1 {
        margin-bottom: 12px;
    }

    p {
        color: #bbbbbb;
    }
</style>
</head>
<body>
    <h1>NppLatexPreview</h1>
    <p>WebView2 is working.</p>
    <p>This panel will eventually display the compiled LaTeX PDF.</p>
</body>
</html>
)HTML";

                            return g_webView->NavigateToString(
                                testHtml
                            );
                        }
                    );

                if (controllerHandler == nullptr)
                    return E_OUTOFMEMORY;

                HRESULT controllerResult =
                    environment->CreateCoreWebView2Controller(
                        g_panel,
                        controllerHandler
                    );

                // Release our own reference.
                // The asynchronous WebView2 operation retains the handler
                // while it needs it.
                controllerHandler->Release();

                return controllerResult;
            }
        );

    if (environmentHandler == nullptr)
    {
        MessageBoxW(
            nppData._nppHandle,
            L"Could not allocate the WebView2 environment callback.",
            PLUGIN_NAME,
            MB_OK | MB_ICONERROR
        );

        return;
    }

    wchar_t localAppData[MAX_PATH];

	DWORD length = GetEnvironmentVariableW(
		L"LOCALAPPDATA",
		localAppData,
		MAX_PATH
	);

	if (length == 0 || length >= MAX_PATH)
	{
		MessageBox(
			g_panel,
			L"Could not determine LOCALAPPDATA.",
			L"NppLatexPreview",
			MB_OK | MB_ICONERROR
		);

		environmentHandler->Release();
		return;
	}

	std::wstring userDataFolder =
		std::wstring(localAppData) +
		L"\\NppLatexPreview\\WebView2";

	HRESULT hr =
		CreateCoreWebView2EnvironmentWithOptions(
			nullptr,
			userDataFolder.c_str(),
			nullptr,
			environmentHandler);

	if (FAILED(hr))
	{
		wchar_t message[256];

		swprintf_s(
			message,
			L"CreateCoreWebView2EnvironmentWithOptions failed.\n\n"
			L"HRESULT: 0x%08lX",
			static_cast<unsigned long>(hr)
		);

		MessageBox(
			g_panel,
			message,
			L"NppLatexPreview",
			MB_OK | MB_ICONERROR
		);

		environmentHandler->Release();
		return;
	}
}


// -----------------------------------------------------------------------------
// Resize WebView2 whenever the docking panel changes size
// -----------------------------------------------------------------------------

static void resizeWebView()
{
    if (!g_controller || !g_panel)
        return;

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

    // The only menu command we currently need.
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
void beNotified(SCNotification*)
{
    // Nothing needed yet.
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