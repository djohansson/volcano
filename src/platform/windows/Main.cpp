#include "Resource.h"

#include <sdkddkver.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <Windows.h>

#include "../../Volcano.h"
#include "../../platform/windows/MessageToString.h"
#include "../../../../imgui/imgui.h"
#include "../../../../imgui/examples/imgui_impl_win32.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DebugStreamBuffer.h"


struct WindowData
{
	enum { TitleStringSize = 128 };
	enum Flags : uint64_t
	{
		Verbose = 1 << 0,
		DrawEnable = 1 << 1,
	};

	WCHAR myTitle[TitleStringSize];
	WCHAR myWindowClass[TitleStringSize];
	LPWSTR myCmdLine = nullptr;
	int myCmdShow = 0;
	HINSTANCE myHInstance = nullptr;
	HWND myHWnd = nullptr;
	HWND myHWndDlgModeless = nullptr;
	HACCEL myHAccelTable = nullptr;
	MSG myMsg = {};
	int myWidth = 0;
	int myHeight = 0;
	std::string myResourcePath;
	uint64_t myFlags = DrawEnable;
};

static thread_local WindowData* t_windowData = nullptr; // hacky way to get this into WndProc...

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

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
		return 1;

#ifdef _DEBUG
	if (const char* msgString = GetStringFromMsg(message))
		std::cout << "message: " << msgString << ", hWnd:" << hWnd << ", wParam: " << wParam << ", lParam: " << lParam << std::endl;
#endif

	WindowData& wnd = *t_windowData;

	switch (message)
	{
	case WM_ENTERSIZEMOVE:
	{
		wnd.myFlags &= ~WindowData::Flags::DrawEnable;
	}
	break;
	case WM_EXITSIZEMOVE:
	{
		wnd.myFlags |= WindowData::Flags::DrawEnable;
	}
	break;
	case WM_PAINT:
	{
		if ((wnd.myFlags & WindowData::DrawEnable) && !IsIconic(hWnd) && vkapp_is_initialized())
		{
			ImGui_ImplWin32_NewFrame();
			vkapp_draw();
		}
	}
	break;
	case WM_GETMINMAXINFO:
	{
		reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { 320, 240 };
	}
	break;
	case WM_SIZE:
	{
		if (!IsIconic(hWnd) && vkapp_is_initialized())
			vkapp_resize(LOWORD(lParam), HIWORD(lParam));
	}
	break;
	case WM_SYSCOMMAND:
	{
		if ((wParam & 0xfff0) != SC_KEYMENU) // Disable ALT application menu
			return DefWindowProc(hWnd, message, wParam, lParam);
	}
	break;
	case WM_DESTROY:
	{
		ImGui_ImplWin32_Shutdown();
		vkapp_destroy();
		PostQuitMessage(0);
	}
	break;
	case WM_CLOSE:
	{
		DestroyWindow(hWnd);
	}
	break;
	case WM_ACTIVATE:
	case WM_ACTIVATEAPP:
	{
		switch (wParam)
		{
		case 0: // FALSE or WM_INACTIVE
			break;
		case 1: // TRUE or WM_ACTIVE or WM_CLICKACTIVE
		default:
			break;
		}
	}
	break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
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
	HWND& hWnd = data->myHWnd;
	HWND& hWndDlgModeless = data->myHWndDlgModeless;
	HACCEL& hAccelTable = data->myHAccelTable;
	MSG& msg = data->myMsg;

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
	LPWSTR& lpCmdLine = data->myCmdLine;
	HINSTANCE& hInstance = data->myHInstance;
	HWND& hWnd = data->myHWnd;
	HACCEL& hAccelTable = data->myHAccelTable;
	int& width = data->myWidth;
	int& height = data->myHeight;
	std::string& resourcePath = data->myResourcePath;
	auto flags = data->myFlags;
	
	// parse commandline options
	{
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

		flags |= cmdOptionExists(argv, argv + argc, "-v") ? WindowData::Verbose : 0;

		char* widthStr = getCmdOption(argv, argv + argc, "-w");
		char* heightStr = getCmdOption(argv, argv + argc, "-h");

		width = widthStr ? atoi(widthStr) : 1920;
		height = heightStr ? atoi(heightStr) : 1080;
		resourcePath = getCmdOption(argv, argv + argc, "-r");

		for (int i = 0; i < argc; i++)
			delete[] argv[i];

		delete[] argv;
	}

	// Initialize global strings
	LoadStringW(hInstance, IDS_APP_TITLE, data->myTitle, WindowData::TitleStringSize);
	LoadStringW(hInstance, IDC_VOLCANO, data->myWindowClass, WindowData::TitleStringSize);

	WNDCLASSEXW wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW | CS_PARENTDC;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VOLCANO));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_VOLCANO);
	wcex.lpszClassName = data->myWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	if (!RegisterClassExW(&wcex))
		throw std::runtime_error("failed to register window!");

	hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_VOLCANO));

	// todo: investigate "On Nvidia you can get Exclusive Fullscreen though, by creating a
	// borderless window (style:0x96000000/ex:0x0) and rendering to it."
	RECT wr = { 0, 0, width, height };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
	hWnd = CreateWindowExW(0, data->myWindowClass, data->myTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
		CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);

	if (!hWnd)
		throw std::runtime_error("failed to initialize window!");

	ShowWindow(hWnd, data->myCmdShow);

	vkapp_create((void*)hWnd, width, height, resourcePath.c_str(), flags & WindowData::Verbose);
	ImGui_ImplWin32_Init(hWnd);

	MessageLoop(data);
}

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
	windowData.myCmdLine = lpCmdLine;
	windowData.myCmdShow = nCmdShow;
	windowData.myHInstance = hInstance;

	t_windowData = &windowData;

	try
	{
		InitializeAndRunMessageLoop(&windowData);
	}
	catch (const std::runtime_error& e)
	{
		const char* str = e.what();
		int strLen = static_cast<int>(strlen(str));
		int wStrSize = MultiByteToWideChar(CP_UTF8, 0, str, strLen, NULL, 0);
		std::wstring wStr(wStrSize, 0);
		MultiByteToWideChar(CP_UTF8, 0, str, strLen, &wStr[0], wStrSize);

		std::wcout << wStr << std::endl;

		if (windowData.myHWnd)
			MessageBox(windowData.myHWnd, wStr.c_str(), L"Failure", MB_OKCANCEL);

		windowData.myMsg.wParam = static_cast<WPARAM>(EXIT_FAILURE);
	}
	catch (...)
	{
		if (windowData.myHWnd)
			MessageBox(windowData.myHWnd, L"Unknown exception", L"Failure", MB_OKCANCEL);

		windowData.myMsg.wParam = static_cast<WPARAM>(EXIT_FAILURE);
	}

	t_windowData = nullptr;

	CleanupDebug();

	return static_cast<int>(windowData.myMsg.wParam);
}
