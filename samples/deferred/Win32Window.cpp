// ============================================================
//  samples/deferred/Win32Window.cpp
// ============================================================
#include "Win32Window.h"
#include <stdexcept>

// ============================================================
//  Win32 ネイティブウィンドウ ヘルパー
// ============================================================
#if defined(_WIN32)


static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	auto* ctx = reinterpret_cast<WndContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_KEYDOWN:
		if (wp == VK_F1 && ctx)
			ctx->debugRT = !ctx->debugRT;
		return 0;

	case WM_SIZE:
	{
		// 最小化 (SIZE_MINIMIZED) のときはリサイズしない
		if (wp == SIZE_MINIMIZED)
			return 0;

		const uint32_t w = LOWORD(lp);
		const uint32_t h = HIWORD(lp);

		// サイズが 0 のときは無視する
		if (w == 0 || h == 0)
			return 0;

		// フレームループ側で処理するためフラグだけ立てる
		// (GPU コマンド実行中に resize を呼ぶと COMMAND_ALLOCATOR_SYNC が発生するため)
		if (ctx)
		{
			ctx->resized = true;
			ctx->newW    = w;
			ctx->newH    = h;
		}
		return 0;
	}

	default:
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

// Win32 ウィンドウを作成して HWND を返す
HWND createNativeWindow(
	HINSTANCE      hInstance,
	const wchar_t* title,
	uint32_t       width,
	uint32_t       height)
{
	WNDCLASSEXW wc      = {};
	wc.cbSize           = sizeof(wc);
	wc.style            = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc      = WndProc;
	wc.hInstance        = hInstance;
	wc.hCursor          = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName    = L"GfxExampleClass";

	if (!RegisterClassExW(&wc))
		throw std::runtime_error("[Error] RegisterClassExW failed");

	// クライアント領域が指定サイズになるようウィンドウ全体のサイズを計算する
	RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExW(
		0,
		L"GfxExampleClass",
		title,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right  - rc.left,
		rc.bottom - rc.top,
		nullptr,
		nullptr,
		hInstance,
		nullptr);

	if (!hwnd)
		throw std::runtime_error("[Error] CreateWindowExW failed");

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	return hwnd;
}

#endif // _WIN32

// ============================================================
//  メッセージループ処理 (1 フレーム分のメッセージをポンプする)
//  戻り値: false = WM_QUIT を受信したため終了すべき
// ============================================================
bool pumpMessages()
{
#if defined(_WIN32)
	MSG msg = {};
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return false;

		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
#endif
	return true;
}
