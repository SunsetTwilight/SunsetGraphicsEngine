#pragma once
// ============================================================
//  GfxBackendABI.h  (RT/DS 分離版)
//
//  責務の分割:
//    ISwapChain    … バックバッファの flip / Present のみ。
//                    RTV/DSV を一切持たない。
//    IRenderTarget … カラー RT (RTV/ImageView) 1 枚。
//                    SwapChain バックバッファ用でも独立 RT 用でも使える。
//    IDepthStencil … デプス/ステンシル (DSV/ImageView) 1 枚。
//                    SwapChain とは独立して作成・破棄できる。
//
//  フレームの流れ:
//    swapChain->acquireNextBuffer()   ← バックバッファ index を確定
//    swapChain->getRenderTarget()     ← そのフレームの IRenderTarget* を取得
//    cmdBuf->setRenderTargets(rt, ds) ← 明示的にバインド
//    ... draw ...
//    cmdBuf->endRenderPass()
//    swapChain->present()             ← flip
//
//  Deferred Rendering:
//    gbuf_albedo = device->createRenderTarget(albedoDesc)
//    gbuf_normal = device->createRenderTarget(normalDesc)
//    gbuf_depth  = device->createDepthStencil(depthDesc)
//    // swapChain は最終 Present 専用
// ============================================================

// ABI 全体を読み込む。各パートの内容は以下のヘッダーを参照。
#include "Plugin.h"   // Types → PipelineState → Commands → Resources → Compat → Interfaces → Device → Plugin
