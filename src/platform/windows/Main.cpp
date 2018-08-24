#include "Resource.h"
#include "targetver.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <windows.h>

#include <crtdbg.h>
#include <malloc.h>
#include <memory.h>
#include <shellapi.h>
#include <stdlib.h>
#include <tchar.h>

#include "../../Volcano.h"
#include "../../../../imgui/imgui.h"
#include "../../../../imgui/examples/imgui_impl_win32.h"

#include <iostream>
#include <streambuf>

#define MAX_LOADSTRING 100

class DebugStreamBuffer : public std::streambuf
{
public:
	virtual int_type overflow(int_type c = EOF)
	{
		if (c != EOF)
		{
			TCHAR buf[] = { static_cast<TCHAR>(c), '\0' };
			OutputDebugString(buf);
		}
		return c;
	}
};

// Global Variables:
HINSTANCE g_hInst;					   // current instance
HWND g_hWnd;						   // main window
WCHAR g_szTitle[MAX_LOADSTRING];	   // The title bar text
WCHAR g_szWindowClass[MAX_LOADSTRING]; // the main window class name
bool g_initialized = false;

int __cdecl CrtDbgHook(int nReportType, char* szMsg, int* pnRet)
{
	if (IsDebuggerPresent())
	{
		_CrtDbgBreak();
		return TRUE;
	}

	return FALSE; // Return false - Abort,Retry,Ignore dialog *will be displayed*
}

// Forward declarations of functions included in this code module:
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
	_CrtSetReportHook(CrtDbgHook);

	DebugStreamBuffer debugStream;
	std::streambuf* defaultCoutStream = std::cout.rdbuf(&debugStream);
	std::streambuf* defaultCerrStream = std::cerr.rdbuf(&debugStream);
	std::streambuf* defaultClogStream = std::clog.rdbuf(&debugStream);

	int argc;
	char** argv;

	// Use the CommandLine functions to get the command line arguments.
	// Unfortunately, Microsoft outputs
	// this information as wide characters for Unicode, and we simply want the
	// Ascii version to be compatible
	// with the non-Windows side.  So, we have to convert the information to
	// Ascii character strings.
	LPWSTR* commandLineArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (NULL == commandLineArgs)
	{
		argc = 0;
	}

	if (argc > 0)
	{
		argv = (char**)malloc(sizeof(char*) * argc);
		if (argv == NULL)
		{
			argc = 0;
		}
		else
		{
			for (int i = 0; i < argc; i++)
			{
				size_t wideCharLen = wcslen(commandLineArgs[i]);
				size_t numConverted = 0;

				argv[i] = (char*)malloc(sizeof(char) * (wideCharLen + 1));
				if (argv[i] != NULL)
				{
					wcstombs_s(&numConverted, argv[i], wideCharLen + 1, commandLineArgs[i],
						wideCharLen + 1);
				}
			}
		}
	}
	else
	{
		argv = NULL;
	}

	// TODO: Place initialization code here.

	// Initialize global strings
	LoadStringW(hInstance, IDS_APP_TITLE, g_szTitle, MAX_LOADSTRING);
	LoadStringW(hInstance, IDC_VOLCANO, g_szWindowClass, MAX_LOADSTRING);

	// Perform application initialization:
	if (!MyRegisterClass(hInstance) || !InitInstance(hInstance, nCmdShow))
	{
		return FALSE;
	}

	vkapp_create((void*)g_hWnd, 1280, 720, "..\\..\\..\\..\\..\\resources\\");
	ImGui_ImplWin32_Init(g_hWnd);
	g_initialized = true;

	// Free up the items we had to allocate for the command line arguments.
	if (argc > 0 && argv != NULL)
	{
		for (int iii = 0; iii < argc; iii++)
		{
			if (argv[iii] != NULL)
			{
				free(argv[iii]);
			}
		}
		free(argv);
	}

	HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_VOLCANO));

	MSG msg;

	// Ensure wParam is initialized.
	msg.wParam = 0;

	// main message loop
	bool done = false; // flag saying when app is complete
	while (!done)
	{
		PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
		if (msg.message == WM_QUIT) // check for a quit message
		{
			done = true; // if found, quit app
		}
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			/* Translate and dispatch to event queue*/
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		RedrawWindow(g_hWnd, NULL, NULL, RDW_INTERNALPAINT);
	}

	vkapp_destroy();

	std::cout.rdbuf(defaultCoutStream);
	std::cerr.rdbuf(defaultCerrStream);
	std::clog.rdbuf(defaultClogStream);

	return (int)msg.wParam;
}

//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
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
	wcex.lpszClassName = g_szWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	g_hInst = hInstance; // Store instance handle in our global variable

	// todo: investigate "On Nvidia you can get Exclusive Fullscreen though, by creating a
	// borderless window (style:0x96000000/ex:0x0) and rendering to it."
	g_hWnd = CreateWindowW(g_szWindowClass, g_szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
		CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);

	if (!g_hWnd)
	{
		return FALSE;
	}

	ShowWindow(g_hWnd, nCmdShow);
	UpdateWindow(g_hWnd);

	return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
		return true;
	//__noop;

	switch (message)
	{
	case WM_COMMAND:
	{
		int wmId = LOWORD(wParam);
		// Parse the menu selections:
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
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
		// The validation callback calls MessageBox which can generate paint
		// events - don't make more Vulkan calls if we got here from the
		// callback
		// if (!in_callback)
		//	demo_run(&demo);
		if (g_initialized)
		{
			ImGui_ImplWin32_NewFrame();
			vkapp_draw();
		}
		break;
	}
	break;
	case WM_GETMINMAXINFO: // set window's minimum size
						   //((MINMAXINFO *)lParam)->ptMinTrackSize = demo.minsize;
		break;
	case WM_SIZE:
		// Resize the application to the new window size, except when
		// it was minimized. Vulkan doesn't support images or swapchains
		// with width=0 and height=0.
		// if (wParam != SIZE_MINIMIZED)
		//{
		//	demo.width = lParam & 0xffff;
		//	demo.height = (lParam & 0xffff0000) >> 16;
		//	demo_resize(&demo);
		//}
		break;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// Message handler for about box.
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

// On MS-Windows, make this a global, so it's available to WndProc()
// struct demo demo;
//
// static void demo_run(struct demo *demo)
//{
//	if (!demo->prepared)
//		return;
//
//	demo_draw(demo);
//	demo->curFrame++;
//
//	if (demo->frameCount != INT_MAX && demo->curFrame == demo->frameCount)
//		PostQuitMessage(validation_error);
//}
