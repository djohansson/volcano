#include "Resource.h"

#include <sdkddkver.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <Windows.h>

#include <crtdbg.h>
#include <shellapi.h>
#include <tchar.h>

#include "../../Volcano.h"
#include "../../../../imgui/imgui.h"
#include "../../../../imgui/examples/imgui_impl_win32.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include <ppl.h>
#include <concurrent_queue.h>

template <typename T>
class DebugStreamBuffer : public std::basic_streambuf<T, std::char_traits<T>>
{
	using Super = std::basic_streambuf<T, std::char_traits<T>>;
	using int_type = typename Super::int_type;
	using OutputDebugStringType = typename std::conditional<std::is_same<char, T>::value, TCHAR, WCHAR>::type;

public:
	virtual int_type overflow(int_type c = EOF)
	{
		if (c != EOF)
		{
			OutputDebugStringType buf[] = { static_cast<OutputDebugStringType>(c), '\0' };
			OutputDebugString(buf);
		}
		return c;
	}
};

class ConcurrentFunctionQueue
{
public:

	ConcurrentFunctionQueue()
		: queue()
	{ }

	template<typename _Callable, typename... _Args>
	void Queue(_Callable&& __f, _Args&&... __args)
	{
		std::function<void()> func = std::bind(std::forward<_Callable>(__f), std::forward<_Args>(__args)...);
		queue.push(func);
	}

	void RunOne()
	{
		std::function<void()> func;
		if (queue.try_pop(func))
			func();
	}

	void RunAll()
	{
		std::function<void()> func;
		while (queue.try_pop(func))
			func();
	}

private:

	concurrency::concurrent_queue<std::function<void()>> queue;
};

struct WindowData
{
	LPWSTR lpCmdLine = nullptr;
	int nCmdShow = 0;
	HINSTANCE hInstance = nullptr;
	HWND hWnd = nullptr;
	HWND hWndDlgModeless = nullptr;
	HACCEL hAccelTable = nullptr;
	MSG msg = {};
	int width = 0;
	int height = 0;
	std::string resourcePath;
	bool verbose = false;

	ConcurrentFunctionQueue runOnRenderThread;

	std::thread msgLoopThread;
	std::thread renderThread;
	std::mutex msgLoopMutex;
	std::mutex renderMutex;
	std::condition_variable windowInitCV;
};

static const unsigned int sc_staticTitleStringSize = 128;
static WCHAR g_title[sc_staticTitleStringSize];
static WCHAR g_windowClass[sc_staticTitleStringSize];
static DebugStreamBuffer<char> g_debugStream;
static DebugStreamBuffer<wchar_t> g_wDebugStream;
static std::streambuf* g_defaultCoutStream = nullptr;
static std::streambuf* g_defaultCerrStream = nullptr;
static std::streambuf* g_defaultClogStream = nullptr;
static std::wstreambuf* g_defaultWCoutStream = nullptr;
static std::wstreambuf* g_defaultWCerrStream = nullptr;
static std::wstreambuf* g_defaultWClogStream = nullptr;
thread_local WindowData* t_windowData = nullptr;


extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int __cdecl CrtDbgHook(int /*nReportType*/, char* /*szMsg*/, int* /*pnRet*/)
{
	if (IsDebuggerPresent())
	{
		_CrtDbgBreak();
		return TRUE;
	}

	return FALSE;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	t_windowData->runOnRenderThread.Queue(ImGui_ImplWin32_WndProcHandler, hWnd, message, wParam, lParam);
	
	switch (message)
	{
	case WM_COMMAND:
	{
		int wmId = LOWORD(wParam);
		switch (wmId)
		{
		case IDM_ABOUT:
			//DialogBox(g_hInstance, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
			break;
		case IDM_EXIT:
			DestroyWindow(hWnd);
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}
	}
	break;
	case WM_PAINT:
	{
		if (!IsIconic(hWnd) && vkapp_is_initialized())
		{
			t_windowData->runOnRenderThread.RunAll();
			ImGui_ImplWin32_NewFrame();
			vkapp_draw();
		}
	}
	break;
	case WM_GETMINMAXINFO: // set window's minimum size
	{
		reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { 320, 240 };
	}
	break;
	case WM_SIZE:
	{
		if (!IsIconic(hWnd) && vkapp_is_initialized())
			t_windowData->runOnRenderThread.Queue(vkapp_resize, LOWORD(lParam), HIWORD(lParam));
	}
	break;
	case WM_SYSCOMMAND:
	{
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
	}
	break;
	case WM_DESTROY:
	{
		PostQuitMessage(0);
	}
	break;
	case WM_CLOSE:
	{
		if (MessageBox(hWnd, L"Really quit?", L"My application", MB_OKCANCEL) == IDOK)
			DestroyWindow(hWnd);
	}
	break;
	case WM_EXITSIZEMOVE:
	{
		//PostMessage(hWnd, WM_RESHAPE, 0, 0);
	}
	break;
	case WM_ACTIVATE:
	{
		//PostMessage(hWnd, WM_ACTIVE, wParam, lParam);
	}
	break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VOLCANO));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_VOLCANO);
	wcex.lpszClassName = g_windowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassExW(&wcex);
}

static char* getCmdOption(char** begin, char** end, const std::string& option)
{
	char** it = std::find(begin, end, option);
	if (it != end && ++it != end)
		return *it;

	return nullptr;
}

static bool cmdOptionExists(char** begin, char** end, const std::string& option)
{
	return std::find(begin, end, option) != end;
}

void MessageLoop(WindowData* data)
{
	std::unique_lock<std::mutex> lck(data->msgLoopMutex);

	HWND& hWnd = data->hWnd;
	HWND& hWndDlgModeless = data->hWndDlgModeless;
	HACCEL& hAccelTable = data->hAccelTable;
	MSG& msg = data->msg;

	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		if (hWndDlgModeless == nullptr ||
			(!IsDialogMessage(hWndDlgModeless, &msg) && !TranslateAccelerator(hWnd, hAccelTable, &msg)))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}

void InitializeDebug()
{
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_WNDW);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_WNDW);
	_CrtSetReportHook(CrtDbgHook);

	g_defaultCoutStream = std::cout.rdbuf(&g_debugStream);
	g_defaultCerrStream = std::cerr.rdbuf(&g_debugStream);
	g_defaultClogStream = std::clog.rdbuf(&g_debugStream);
	g_defaultWCoutStream = std::wcout.rdbuf(&g_wDebugStream);
	g_defaultWCerrStream = std::wcerr.rdbuf(&g_wDebugStream);
	g_defaultWClogStream = std::wclog.rdbuf(&g_wDebugStream);
}

void InitializeAndRunMessageLoop(WindowData* data)
{
	t_windowData = data;

	LPWSTR& lpCmdLine = data->lpCmdLine;
	int& nCmdShow = data->nCmdShow;
	HINSTANCE& hInstance = data->hInstance;
	HWND& hWnd = data->hWnd;
	HACCEL& hAccelTable = data->hAccelTable;
	int& width = data->width;
	int& height = data->height;
	std::string& resourcePath = data->resourcePath;
	bool& verbose = data->verbose;
	
	int argc = 0;
	LPWSTR* commandLineArgs = CommandLineToArgvW(lpCmdLine, &argc);
	assert(commandLineArgs != nullptr);
	
	char** argv = new char*[argc];
	for (int i = 0; i < argc; i++)
	{
		size_t wideCharLen = wcslen(commandLineArgs[i]);
		size_t numConverted = 0;
		argv[i] = new char[wideCharLen + 1];
		wcstombs_s(&numConverted, argv[i], wideCharLen + 1, commandLineArgs[i], wideCharLen + 1);
	}
	LocalFree(commandLineArgs);

	verbose = cmdOptionExists(argv, argv + argc, "-v");

	char* widthStr = getCmdOption(argv, argv + argc, "-w");
	char* heightStr = getCmdOption(argv, argv + argc, "-h");

	width = widthStr ? atoi(widthStr) : 1920;
	height = heightStr ? atoi(heightStr) : 1080;
	resourcePath = getCmdOption(argv, argv + argc, "-r");

	// Initialize global strings
	LoadStringW(hInstance, IDS_APP_TITLE, g_title, sc_staticTitleStringSize);
	LoadStringW(hInstance, IDC_VOLCANO, g_windowClass, sc_staticTitleStringSize);

	if (!MyRegisterClass(hInstance))
		throw std::runtime_error("failed to register window!");

	hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_VOLCANO));

	// todo: investigate "On Nvidia you can get Exclusive Fullscreen though, by creating a
	// borderless window (style:0x96000000/ex:0x0) and rendering to it."
	RECT wr = { 0, 0, width, height };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, TRUE);
	hWnd = CreateWindow(g_windowClass, g_title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
		CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);

	if (!hWnd)
		throw std::runtime_error("failed to initialize window!");

	ShowWindow(hWnd, nCmdShow);

	for (int i = 0; i < argc; i++)
		delete[] argv[i];

	delete[] argv;

	data->windowInitCV.notify_all();

	// TEMP
	vkapp_create((void*)hWnd, width, height, resourcePath.c_str(), verbose);
	ImGui_ImplWin32_Init(hWnd);
	// TEMP

	MessageLoop(data);

	// TEMP
	ImGui_ImplWin32_Shutdown();
	vkapp_destroy();
	// TEMP

	t_windowData = nullptr;
}

//void InitializeAndRunRenderThread(WindowData* data)
//{
//	t_windowData = data;
//
//	std::unique_lock<std::mutex> lock(data->renderMutex);
//	data->windowInitCV.wait(lock);
//
//	const HWND& hWnd = data->hWnd;
//	const int width = data->width;
//	const int height = data->height;
//	const std::string& resourcePath = data->resourcePath;
//	const bool verbose = data->verbose;
//
//	vkapp_create((void*)hWnd, width, height, resourcePath.c_str(), verbose);
//	ImGui_ImplWin32_Init(hWnd);
//
//	while (true)
//	{
//		if (!IsIconic(hWnd) && vkapp_is_initialized())
//		{
//			t_windowData->runOnRenderThread.RunAll();
//			ImGui_ImplWin32_NewFrame();
//			vkapp_draw();
//		}
//	}
//
//	ImGui_ImplWin32_Shutdown();
//	vkapp_destroy();
//
//	t_windowData = nullptr;
//}

void CleanupDebug()
{
	std::cout.rdbuf(g_defaultCoutStream);
	std::cerr.rdbuf(g_defaultCerrStream);
	std::clog.rdbuf(g_defaultClogStream);
	std::wcout.rdbuf(g_defaultWCoutStream);
	std::wcerr.rdbuf(g_defaultWCerrStream);
	std::wclog.rdbuf(g_defaultWClogStream);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	InitializeDebug();

	WindowData windowData = {};
	windowData.lpCmdLine = lpCmdLine;
	windowData.nCmdShow = nCmdShow;
	windowData.hInstance = hInstance;

	try
	{
		windowData.msgLoopThread = std::thread(InitializeAndRunMessageLoop, &windowData);
		//windowData.renderThread = std::thread(InitializeAndRunRenderThread, &windowData);

		windowData.msgLoopThread.join();
		//windowData.renderThread.join();
	}
	catch (const std::runtime_error& e)
	{
		const char* str = e.what();
		int strLen = static_cast<int>(strlen(str));
		int wStrSize = MultiByteToWideChar(CP_UTF8, 0, str, strLen, NULL, 0);
		std::wstring wStr(wStrSize, 0);
		MultiByteToWideChar(CP_UTF8, 0, str, strLen, &wStr[0], wStrSize);

		std::wcout << wStr << std::endl;

		if (windowData.hWnd)
			MessageBox(windowData.hWnd, wStr.c_str(), L"Failure", MB_OKCANCEL);

		windowData.msg.wParam = static_cast<WPARAM>(EXIT_FAILURE);
	}
	catch (...)
	{
		if (windowData.hWnd)
			MessageBox(windowData.hWnd, L"Unknown exception", L"Failure", MB_OKCANCEL);

		windowData.msg.wParam = static_cast<WPARAM>(EXIT_FAILURE);
	}

	CleanupDebug();

	return static_cast<int>(windowData.msg.wParam);
}
