#pragma once
// ============================================================
//  GraphicsEngine.h  (RT/DS 分離版)
// ============================================================

#include "GfxBackendABI.h"
#include "PluginLoader.h"
#include <string>
#include <vector>
#include <functional>
#include <initializer_list>

// ============================================================
//  アプリ向け高レベル記述子
// ============================================================
struct DeviceConfig {
	GraphicsAPI api        = GraphicsAPI::Auto;
	bool        validation = false;
	std::string appName    = "GraphicsApp";

	GfxDeviceConfig toPOD() const {
		GfxDeviceConfig c; c.api = api; c.validation = validation;
		auto n = appName.substr(0, 127); n.copy(c.appName, n.size()); c.appName[n.size()] = '\0';
		return c;
	}
};

struct WindowDesc {
	void*       nativeWindowHandle = nullptr;
	uint32_t    width  = 1280, height = 720;
	bool        vsync  = true;
	PixelFormat format = PixelFormat::BGRA8_UNORM;
	uint32_t    bufferCount = 2;
	std::string name   = "Window";

	GfxSwapChainDesc toPOD() const {
		GfxSwapChainDesc d;
		d.nativeWindowHandle = nativeWindowHandle;
		d.width = width; d.height = height;
		d.vsync = vsync; d.format = format; d.bufferCount = bufferCount;
		return d;
	}
};

// アプリ向けエイリアス
using RenderTargetDesc = GfxRenderTargetDesc;
using DepthStencilDesc = GfxDepthStencilDesc;
using BufferDesc       = GfxBufferDesc;
using TextureDesc      = GfxTextureDesc;
using ShaderDesc       = GfxShaderDesc;
using DrawCmd          = GfxDrawCmd;

// パイプライン設定で使う列挙型を GraphicsEngine の名前空間に公開する
using Topology         = ::Topology;
using BlendMode        = ::BlendMode;
using CullMode         = ::CullMode;
using FillMode         = ::FillMode;
using FilterMode       = ::FilterMode;
using AddressMode      = ::AddressMode;
using ShaderVisibility = ::ShaderVisibility;
using RootParamType    = ::RootParamType;
using VertexAttribute  = ::VertexAttribute;
using BufferUsage      = ::BufferUsage;
using PixelFormat      = ::PixelFormat;
using ShaderStage      = ::ShaderStage;

// ============================================================
//  RenderWindow  (SwapChain のラッパー)
//  Present とバックバッファ RT の取得のみを担う
// ============================================================
class RenderWindow {
public:
	// acquireNextBuffer: フレーム開始。GPU フェンス待機 + バックバッファ index 確定。
	bool acquireNextBuffer();

	// getRenderTarget: 現フレームのバックバッファ RT を返す。
	//   acquireNextBuffer() 後に有効。setRenderTargets() に渡して使う。
	//   所有権は RenderWindow 側にあるため delete してはいけない。
	IRenderTarget* getRenderTarget();

	// present: 画面に表示。
	void present();

	// リサイズ (WM_SIZE 等から呼ぶ)
	bool resize(uint32_t w, uint32_t h);

	uint32_t    width()  const;
	uint32_t    height() const;
	PixelFormat format() const;
	std::string name()   const { return m_desc.name; }
	bool        isValid() const { return m_swapChain != nullptr; }

private:
	friend class GraphicsEngine;
	RenderWindow(ISwapChain* sc, const WindowDesc& desc)
		: m_swapChain(sc), m_desc(desc) {}

	ISwapChain* m_swapChain = nullptr;
	WindowDesc  m_desc;   // switchAPI でスワップチェーンを作り直すために保持する (サイズは resize で更新)
};

// ============================================================
//  GraphicsEngine
// ============================================================
class GraphicsEngine {
public:
	GraphicsEngine();
	~GraphicsEngine();
	GraphicsEngine(const GraphicsEngine&)            = delete;
	GraphicsEngine& operator=(const GraphicsEngine&) = delete;

	// -------------------------------------------------------
	//  デバイス初期化 / 終了
	// -------------------------------------------------------
	void addPluginSearchPath(const std::string& dir);
	bool init    (const DeviceConfig& cfg);
	void shutdown();

	// バックエンドを実行時に切り替える。
	//   ・ウィンドウ (RenderWindow) はスワップチェーンを作り直して引き継がれる
	//   ・それ以外のリソース (RT / DS / Buffer / Texture / Pipeline / CommandBuffer 等) は
	//     旧バックエンドのものなので、preSwap コールバックの中でアプリが破棄すること。
	//     postSwap コールバックで新しいバックエンドに作り直す。
	//   ・失敗した場合 false を返し、エンジンは未初期化状態になる (init() からやり直す)
	bool switchAPI(GraphicsAPI newAPI);

	// -------------------------------------------------------
	//  ウィンドウ管理 (SwapChain)
	// -------------------------------------------------------
	RenderWindow* createWindow   (const WindowDesc& desc);
	void          destroyWindow  (RenderWindow* win);
	const std::vector<RenderWindow*>& windows() const { return m_windows; }

	// -------------------------------------------------------
	//  RenderTarget / DepthStencil 管理
	//  デバイスに属し、どのパスからも使い回せる。
	// -------------------------------------------------------
	IRenderTarget* createRenderTarget(const RenderTargetDesc& desc);
	IDepthStencil* createDepthStencil(const DepthStencilDesc& desc);

	// RT/DS の破棄 (DLL 内部で解放。GPU が使用中なら完了後に遅延解放される)
	// ※ RenderWindow::getRenderTarget() が返すバックバッファは破棄できない
	void destroyRenderTarget(IRenderTarget* rt);
	void destroyDepthStencil(IDepthStencil* ds);

	// -------------------------------------------------------
	//  コマンドバッファ管理
	//
	//  allocCommandBuffer() でコマンドバッファを確保し、
	//  外部で begin/end/setRenderTargets/draw を呼べる。
	//  フレームが終わったら submitCommandBuffers() でサブミットし、
	//  不要になったら release() で解放する。
	//
	//  典型的な使い方:
	//    auto* cb = engine.allocCommandBuffer();
	//    cb->begin();
	//    cb->setRenderTargets({ backRT }, depthDS);
	//    cb->bindShader(vs); cb->draw(cmd);
	//    cb->end();
	//    engine.submitCommandBuffers({ cb });
	//    cb->release();
	//
	//  Bundle (DX12 Bundle / Vulkan Secondary):
	//    auto* bundle = engine.allocCommandBuffer(CommandBufferType::Bundle);
	//    bundle->begin();
	//    bundle->bindShader(vs); bundle->draw(cmd);
	//    bundle->end();
	//    // メインパスで再利用
	//    cb->begin(); bundle->executeBundle(cb); cb->end();
	//    engine.submitCommandBuffers({ cb });
	// -------------------------------------------------------
	ICommandBuffer* allocCommandBuffer(
		CommandBufferType type = CommandBufferType::Direct);

	void submitCommandBuffers(
		std::initializer_list<ICommandBuffer*> cbs);

	// -------------------------------------------------------
	//  GPU リソース
	// -------------------------------------------------------
	BufferHandle  createBuffer (const BufferDesc&    desc);
	TextureHandle createTexture(const TextureDesc&   desc);
	ShaderHandle  compileShader(const ShaderDesc&    desc);
	SamplerHandle  createSampler  (const GfxSamplerDesc&  desc);
	PipelineHandle createPipeline (const GfxPipelineDesc& desc);
	// ABI v2: D3D12 の構造体と同じ粒度の記述子
	PipelineHandle      createPipeline      (const GfxGraphicsPipelineDesc& desc);
	BindingLayoutHandle createBindingLayout (const GfxBindingLayoutDesc&    desc);
	void                destroyBindingLayout(BindingLayoutHandle h);
	PipelineHandle      createPipeline      (const GfxComputePipelineDesc&  desc);
	// クエリ (タイムスタンプ / オクルージョン)
	QueryPoolHandle     createQueryPool      (const GfxQueryPoolDesc& desc);
	void                destroyQueryPool     (QueryPoolHandle h);
	bool                readQueryResults     (QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out);
	uint64_t            getTimestampFrequency() const;
	// テクスチャ / ビュー / サンプラーの v2 記述子 (破棄は destroyTexture / destroyView / destroySampler)
	TextureHandle       createTexture        (const GfxTextureResourceDesc& desc);
	ViewHandle          createView           (const GfxViewDesc& desc);
	void                destroyView          (ViewHandle h);
	SamplerHandle       createSampler        (const GfxSamplerStateDesc& desc);
	void destroyBuffer   (BufferHandle   h);
	void destroyTexture  (TextureHandle  h);
	void destroyShader   (ShaderHandle   h);
	void destroySampler  (SamplerHandle  h);
	void destroyPipeline (PipelineHandle h);

	// Raytracing 加速構造
	BLASHandle createBLAS  (const GfxBLASDesc& desc);
	void       destroyBLAS (BLASHandle h);
	TLASHandle createTLAS  (const GfxTLASInstance* instances, uint32_t count);
	void       destroyTLAS (TLASHandle h);
	void       updateTLAS  (TLASHandle h, const GfxTLASInstance* instances, uint32_t count);

	RTPipelineHandle createRTPipeline  (const GfxRTPipelineDesc& desc);
	void             destroyRTPipeline (RTPipelineHandle h);

	using BLASDesc       = GfxBLASDesc;
	using TLASInstance   = GfxTLASInstance;
	using RTPipelineDesc = GfxRTPipelineDesc;

	// UPLOAD ヒープのバッファ (Uniform / Staging) にデータを書き込む
	bool updateBuffer(BufferHandle h, const void* data, uint64_t size);

	// -------------------------------------------------------
	//  描画コマンド
	//  ICommandBuffer を直接呼ぶことを推奨。
	//  以下は互換性のためエンジンが内部コマンドバッファへ
	//  フォワードするショートカット。
	// -------------------------------------------------------
	void bindTexture   (TextureHandle h, uint32_t slot);
	void draw          (const DrawCmd& cmd);
	void drawFullscreen();

	// -------------------------------------------------------
	//  クエリ
	// -------------------------------------------------------
	GraphicsAPI currentAPI()    const;
	std::string adapterName()   const;
	uint64_t    vramUsageMB()   const;
	bool        isInitialized() const { return m_initialized; }

	using SwapCallback = std::function<void(GraphicsAPI, GraphicsAPI)>;
	void setPreSwapCallback (SwapCallback cb) { m_preSwap  = std::move(cb); }
	void setPostSwapCallback(SwapCallback cb) { m_postSwap = std::move(cb); }

private:
	bool        loadPlugin(GraphicsAPI api);
	void        unloadPlugin();
	std::string dllBaseName(GraphicsAPI api) const;
	std::string resolveDllPath(const std::string& base) const;

	PluginLoader               m_loader;
	IBackendPlugin*            m_plugin      = nullptr;
	DeviceConfig               m_config;
	bool                       m_initialized = false;
	std::vector<RenderWindow*> m_windows;
	std::vector<std::string>   m_searchPaths;
	SwapCallback               m_preSwap, m_postSwap;
};
