#pragma once
// ============================================================
//  samples/deferred/Win32Window.h
//  サンプル用の Win32 ウィンドウとメッセージループ
// ============================================================
#include "gfx/core/GraphicsEngine.h"
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// SetWindowLongPtr(GWLP_USERDATA) でウィンドウに関連付けるコンテキスト
struct WndContext
{
	RenderWindow*  win      = nullptr;
	IDepthStencil** fwdDS   = nullptr;
	GraphicsEngine* engine  = nullptr;
	bool           resized  = false;
	uint32_t       newW     = 0;
	uint32_t       newH     = 0;
	bool           debugRT  = false;  // F1: RT Shadow テクスチャを表示
};

// Win32 ウィンドウを作成して HWND を返す (失敗時は例外)
HWND createNativeWindow(HINSTANCE hInstance, const wchar_t* title, uint32_t width, uint32_t height);
#endif // _WIN32

// 1 フレーム分のメッセージをポンプする。false = WM_QUIT を受信したため終了すべき
bool pumpMessages();
