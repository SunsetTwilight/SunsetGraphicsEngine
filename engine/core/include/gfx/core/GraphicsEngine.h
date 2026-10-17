#pragma once
// ============================================================
//  gfx/core/GraphicsEngine.h
//
//  エンジン本体 (SunsetGraphicsEngine.dll) の C++ API。
//  ヘッダーのみのラッパーで、すべてアプリ側でコンパイルされる。
//
//  ・エンジンの寿命 / ウィンドウ / switchAPI は本体 DLL の C ABI (gfx/core/CoreAPI.h) を呼ぶ
//  ・リソースの作成や描画は、本体が返すデバイスの vtable (gfx/abi) を直接呼ぶ
//  std::string / std::vector / std::function はアプリ側にだけ存在し、DLL の境界を越えない。
//  そのため、アプリと本体 DLL のコンパイラ / CRT / Debug・Release が違っても動く。
// ============================================================

#include "gfx/abi/GfxBackendABI.h"
#include "gfx/core/CoreAPI.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
//  アプリ向け高レベル記述子
// ============================================================
struct DeviceConfig {
	GraphicsAPI    api        = GraphicsAPI::Auto;
	bool           validation = false;   // デバッグレイヤー (Debug ビルドのバックエンドのみ有効)
	std::string    appName    = "GraphicsApp";
	GfxAdapterType adapter    = GfxAdapterType::Hardware;   // Software = WARP (GPU のない CI など)

	// validation 有効時のデバッグメッセージの受け取り先。空なら stderr に出力される。
	// メッセージを発生させた API を呼んだスレッドから呼ばれる。
	std::function<void(GfxMessageSeverity, int32_t id, const char* text)> debugMessage;

	// コールバック以外を POD に変換する (コールバックは GraphicsEngine::init が設定する)
	GfxDeviceConfig toPOD() const {
		GfxDeviceConfig c; c.api = api; c.validation = validation; c.adapterType = adapter;
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
//  RenderWindow  (本体 DLL のウィンドウのラッパー)
//  Present とバックバッファ RT の取得のみを担う。
//  スワップチェーンは switchAPI で作り直されるため、呼び出しのたびに本体から取得する。
// ============================================================
class RenderWindow {
public:
	// acquireNextBuffer: フレーム開始。GPU フェンス待機 + バックバッファ index 確定。
	bool acquireNextBuffer()
	{
		ISwapChain* sc = swapChain();
		return sc ? sc->acquireNextBuffer() : false;
	}

	// getRenderTarget: 現フレームのバックバッファ RT を返す。
	//   acquireNextBuffer() 後に有効。setRenderTargets() に渡して使う。
	//   所有権はスワップチェーン側にあるため destroy / delete してはいけない。
	IRenderTarget* getRenderTarget()
	{
		ISwapChain* sc = swapChain();
		return sc ? sc->getRenderTarget() : nullptr;
	}

	// present: 画面に表示。
	void present()
	{
		if (ISwapChain* sc = swapChain()) sc->present();
	}

	// リサイズ (WM_SIZE 等から呼ぶ)。最小化 (0x0) は無視される
	bool resize(uint32_t w, uint32_t h) { return gfxWindowResize(m_handle, w, h); }

	uint32_t    width()  const { ISwapChain* sc = swapChain(); return sc ? sc->getWidth()  : 0; }
	uint32_t    height() const { ISwapChain* sc = swapChain(); return sc ? sc->getHeight() : 0; }
	PixelFormat format() const { ISwapChain* sc = swapChain(); return sc ? sc->getFormat() : PixelFormat::Unknown; }
	std::string name()   const { return m_name; }
	bool        isValid() const { return swapChain() != nullptr; }

	// 本体 DLL のウィンドウハンドル (C ABI を直接使う場合)
	GfxWindow* handle() const { return m_handle; }

private:
	friend class GraphicsEngine;
	RenderWindow(GfxWindow* handle, std::string name)
		: m_handle(handle), m_name(std::move(name)) {}

	ISwapChain* swapChain() const { return gfxWindowGetSwapChain(m_handle); }

	GfxWindow*  m_handle = nullptr;
	std::string m_name;
};

// ============================================================
//  GraphicsEngine
// ============================================================
class GraphicsEngine {
public:
	GraphicsEngine() : m_engine(gfxEngineCreate()) {}
	~GraphicsEngine()
	{
		shutdown();
		gfxEngineDestroy(m_engine);
	}
	GraphicsEngine(const GraphicsEngine&)            = delete;
	GraphicsEngine& operator=(const GraphicsEngine&) = delete;

	// -------------------------------------------------------
	//  デバイス初期化 / 終了
	// -------------------------------------------------------
	void addPluginSearchPath(const std::string& dir) { gfxEngineAddPluginSearchPath(m_engine, dir.c_str()); }

	// アプリをビルドしたヘッダーと本体 DLL の ABI が一致しない場合は false を返す
	bool init(const DeviceConfig& cfg)
	{
		GfxABIInfo dllInfo = {};
		gfxCoreGetABIInfo(&dllInfo);
		if (!(dllInfo == gfxCurrentABIInfo()))
		{
			std::cerr << "[Engine] SunsetGraphicsEngine の DLL とヘッダーの ABI が一致しません (DLL abi="
					  << dllInfo.abiVersion << ", header abi=" << GFX_ABI_VERSION << ")\n";
			return false;
		}
		GfxDeviceConfig pod = cfg.toPOD();
		m_debugMessage = cfg.debugMessage;
		if (m_debugMessage)
		{
			pod.debugCallback = &GraphicsEngine::debugMessageThunk;
			pod.debugUser     = this;
		}
		return gfxEngineInit(m_engine, &pod);
	}

	// ウィンドウもすべて破棄する
	void shutdown()
	{
		for (auto* w : m_windows)
			delete w;   // 本体側のウィンドウは gfxEngineShutdown がまとめて破棄する
		m_windows.clear();
		gfxEngineShutdown(m_engine);
	}

	// バックエンドを実行時に切り替える。
	//   ・ウィンドウ (RenderWindow) はスワップチェーンを作り直して引き継がれる
	//   ・それ以外のリソース (RT / DS / Buffer / Texture / Pipeline / CommandBuffer 等) は
	//     旧バックエンドのものなので、preSwap コールバックの中でアプリが破棄すること。
	//     postSwap コールバックで新しいバックエンドに作り直す。
	//   ・失敗した場合 false を返し、エンジンは未初期化状態になる (init() からやり直す)
	bool switchAPI(GraphicsAPI newAPI)
	{
		return gfxEngineSwitchAPI(m_engine, newAPI, &GraphicsEngine::preSwapThunk,
		                          &GraphicsEngine::postSwapThunk, this);
	}

	// -------------------------------------------------------
	//  ウィンドウ管理 (SwapChain)
	// -------------------------------------------------------
	RenderWindow* createWindow(const WindowDesc& desc)
	{
		const GfxSwapChainDesc pod = desc.toPOD();
		GfxWindow* h = gfxEngineCreateWindow(m_engine, &pod, desc.name.c_str());
		if (!h)
			return nullptr;
		auto* win = new RenderWindow(h, desc.name);
		m_windows.push_back(win);
		return win;
	}

	void destroyWindow(RenderWindow* win)
	{
		auto it = std::find(m_windows.begin(), m_windows.end(), win);
		if (it == m_windows.end())
			return;
		gfxEngineDestroyWindow(m_engine, win->m_handle);
		m_windows.erase(it);
		delete win;
	}

	const std::vector<RenderWindow*>& windows() const { return m_windows; }

	// -------------------------------------------------------
	//  RenderTarget / DepthStencil 管理
	//  デバイスに属し、どのパスからも使い回せる。
	// -------------------------------------------------------
	IRenderTarget* createRenderTarget(const RenderTargetDesc& desc) { auto* d = device(); return d ? d->createRenderTarget(desc) : nullptr; }
	IDepthStencil* createDepthStencil(const DepthStencilDesc& desc) { auto* d = device(); return d ? d->createDepthStencil(desc) : nullptr; }

	// RT/DS の破棄 (DLL 内部で解放。GPU が使用中なら完了後に遅延解放される)
	// ※ RenderWindow::getRenderTarget() が返すバックバッファは破棄できない
	void destroyRenderTarget(IRenderTarget* rt) { if (rt && device()) rt->destroy(); }
	void destroyDepthStencil(IDepthStencil* ds) { if (ds && device()) ds->destroy(); }

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
	//    cb->setRenderTargets(1, &backRT, depthDS);
	//    cb->bindPipeline(pipe); cb->draw(cmd);
	//    cb->end();
	//    engine.submitCommandBuffers({ cb });
	//    cb->release();
	//
	//  Bundle (DX12 Bundle / Vulkan Secondary):
	//    auto* bundle = engine.allocCommandBuffer(CommandBufferType::Bundle);
	//    bundle->begin(); bundle->bindPipeline(pipe); bundle->draw(cmd); bundle->end();
	//    // メインパスで再利用
	//    cb->begin(); bundle->executeBundle(cb); cb->end();
	//    engine.submitCommandBuffers({ cb });
	// -------------------------------------------------------
	ICommandBuffer* allocCommandBuffer(CommandBufferType type = CommandBufferType::Direct)
	{
		auto* d = device();
		return d ? d->allocCommandBuffer(type) : nullptr;
	}

	void submitCommandBuffers(std::initializer_list<ICommandBuffer*> cbs)
	{
		if (auto* d = device())
			d->submitCommandBuffers(cbs.begin(), static_cast<uint32_t>(cbs.size()));
	}

	// -------------------------------------------------------
	//  GPU リソース
	//  バックエンド未ロード (init 前 / switchAPI 失敗後) は無効ハンドルを返し、破棄は何もしない
	// -------------------------------------------------------
	BufferHandle   createBuffer  (const BufferDesc&     d) { auto* p = device(); return p ? p->createBuffer(d)   : BufferHandle{};   }
	TextureHandle  createTexture (const TextureDesc&    d) { auto* p = device(); return p ? p->createTexture(d)  : TextureHandle{};  }
	ShaderHandle   compileShader (const ShaderDesc&     d) { auto* p = device(); return p ? p->compileShader(d)  : ShaderHandle{};   }

	// シェーダーのリフレクション (out は大きいのでヒープに置くこと)。リフレクションがなければ false
	bool getShaderReflection(ShaderHandle h, GfxShaderReflection& out)
	{
		auto* p = device();
		out.structSize = static_cast<uint32_t>(sizeof(GfxShaderReflection));
		return p && p->getShaderReflection(h, out);
	}
	// シェーダーが #include で読み込んだファイルのパス (ホットリロードの監視対象)
	std::vector<std::string> shaderDependencies(ShaderHandle h)
	{
		std::vector<std::string> result;
		auto* p = device();
		if (!p)
			return result;
		const uint32_t size = p->getShaderDependencies(h, nullptr, 0);
		if (size <= 1)
			return result;
		std::string buf(size, '\0');
		if (p->getShaderDependencies(h, buf.data(), size) != size)
			return result;
		buf.resize(size - 1);
		size_t start = 0;
		while (start <= buf.size())
		{
			const size_t end = buf.find('\n', start);
			const std::string line = buf.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty())
				result.push_back(line);
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		return result;
	}
	SamplerHandle  createSampler (const GfxSamplerDesc& d) { auto* p = device(); return p ? p->createSampler(d)  : SamplerHandle{};  }
	PipelineHandle createPipeline(const GfxPipelineDesc& d) { auto* p = device(); return p ? p->createPipeline(d) : PipelineHandle{}; }
	// ABI v2: D3D12 の構造体と同じ粒度の記述子
	PipelineHandle      createPipeline      (const GfxGraphicsPipelineDesc& d) { auto* p = device(); return p ? p->createPipeline(d)      : PipelineHandle{};      }
	BindingLayoutHandle createBindingLayout (const GfxBindingLayoutDesc&    d) { auto* p = device(); return p ? p->createBindingLayout(d) : BindingLayoutHandle{}; }
	void                destroyBindingLayout(BindingLayoutHandle h)            { if (auto* p = device()) p->destroyBindingLayout(h); }
	PipelineHandle      createPipeline      (const GfxComputePipelineDesc&  d) { auto* p = device(); return p ? p->createPipeline(d)      : PipelineHandle{};      }

	// ハンドルはそのままで中身を作り直す。失敗したら何も変えずに false (理由はデバッグメッセージ)
	//   updateShader   : シェーダーを再コンパイルし、それを使うパイプラインも作り直す (ホットリロード)
	//   updatePipeline : 記述子を変えて PSO を作り直す (エディターでのステートの変更)
	bool updateShader  (ShaderHandle h, const ShaderDesc& d)                { auto* p = device(); return p && p->updateShader(h, d);   }
	bool updatePipeline(PipelineHandle h, const GfxGraphicsPipelineDesc& d) { auto* p = device(); return p && p->updatePipeline(h, d); }
	bool updatePipeline(PipelineHandle h, const GfxComputePipelineDesc& d)  { auto* p = device(); return p && p->updatePipeline(h, d); }
	// パイプラインの今の記述子 (debugName はパイプラインが生きている間だけ有効)
	bool getPipelineDesc(PipelineHandle h, GfxGraphicsPipelineDesc& out) const { auto* p = device(); return p && p->getPipelineDesc(h, out); }
	bool getPipelineDesc(PipelineHandle h, GfxComputePipelineDesc& out) const  { auto* p = device(); return p && p->getPipelineDesc(h, out); }
	// クエリ (タイムスタンプ / オクルージョン)
	QueryPoolHandle createQueryPool (const GfxQueryPoolDesc& d) { auto* p = device(); return p ? p->createQueryPool(d) : QueryPoolHandle{}; }
	void            destroyQueryPool(QueryPoolHandle h)         { if (auto* p = device()) p->destroyQueryPool(h); }
	bool            readQueryResults(QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out)
	                { auto* p = device(); return p && p->readQueryResults(h, first, count, out); }
	uint64_t        getTimestampFrequency() const { auto* p = device(); return p ? p->getTimestampFrequency() : 0; }
	// テクスチャ / ビュー / サンプラーの v2 記述子 (破棄は destroyTexture / destroyView / destroySampler)
	TextureHandle createTexture(const GfxTextureResourceDesc& d) { auto* p = device(); return p ? p->createTexture(d) : TextureHandle{}; }
	ViewHandle    createView   (const GfxViewDesc& d)            { auto* p = device(); return p ? p->createView(d)    : ViewHandle{};    }
	void          destroyView  (ViewHandle h)                    { if (auto* p = device()) p->destroyView(h); }
	SamplerHandle createSampler(const GfxSamplerStateDesc& d)    { auto* p = device(); return p ? p->createSampler(d) : SamplerHandle{}; }

	void destroyBuffer  (BufferHandle   h) { if (auto* p = device()) p->destroyBuffer(h);   }
	void destroyTexture (TextureHandle  h) { if (auto* p = device()) p->destroyTexture(h);  }
	void destroyShader  (ShaderHandle   h) { if (auto* p = device()) p->destroyShader(h);   }
	void destroySampler (SamplerHandle  h) { if (auto* p = device()) p->destroySampler(h);  }
	void destroyPipeline(PipelineHandle h) { if (auto* p = device()) p->destroyPipeline(h); }

	// Raytracing 加速構造
	BLASHandle createBLAS (const GfxBLASDesc& d)                          { auto* p = device(); return p ? p->createBLAS(d) : BLASHandle{}; }
	void       destroyBLAS(BLASHandle h)                                  { if (auto* p = device()) p->destroyBLAS(h); }
	TLASHandle createTLAS (const GfxTLASInstance* inst, uint32_t n)       { auto* p = device(); return p ? p->createTLAS(inst, n) : TLASHandle{}; }
	void       destroyTLAS(TLASHandle h)                                  { if (auto* p = device()) p->destroyTLAS(h); }
	void       updateTLAS (TLASHandle h, const GfxTLASInstance* inst, uint32_t n) { if (auto* p = device()) p->updateTLAS(h, inst, n); }

	RTPipelineHandle createRTPipeline (const GfxRTPipelineDesc& d) { auto* p = device(); return p ? p->createRTPipeline(d) : RTPipelineHandle{}; }
	void             destroyRTPipeline(RTPipelineHandle h)         { if (auto* p = device()) p->destroyRTPipeline(h); }

	using BLASDesc       = GfxBLASDesc;
	using TLASInstance   = GfxTLASInstance;
	using RTPipelineDesc = GfxRTPipelineDesc;

	// BufferUsage::Readback のバッファを読む (copyBuffer を含む submit が GPU で完了していなければ false。待たない)
	bool readBuffer(BufferHandle h, uint64_t offset, uint64_t size, void* out)
	{
		auto* p = device();
		return p && p->readBuffer(h, offset, size, out);
	}

	// バッファにデータを書き込む (Uniform はバージョンを切り替えて実行中のフレームと競合しない)
	bool updateBuffer(BufferHandle h, const void* data, uint64_t size)
	{
		auto* p = device();
		return p ? p->updateBuffer(h, data, size) : false;
	}

	// -------------------------------------------------------
	//  描画コマンド
	//  ICommandBuffer を直接呼ぶことを推奨。
	//  以下は互換性のためのショートカット (DX12 バックエンドでは何もしない)。
	// -------------------------------------------------------
	void bindTexture   (TextureHandle h, uint32_t slot) { if (auto* p = device()) p->bindTexture(h, slot); }
	void draw          (const DrawCmd& cmd)             { if (auto* p = device()) p->draw(cmd);           }
	void drawFullscreen()                               { if (auto* p = device()) p->drawFullscreen();    }

	// -------------------------------------------------------
	//  クエリ
	// -------------------------------------------------------
	GraphicsAPI currentAPI()    const { auto* p = device(); return p ? p->getAPI() : GraphicsAPI::Auto; }
	std::string adapterName()   const { auto* p = device(); return p ? std::string(p->getAdapterName()) : std::string(); }
	uint64_t    vramUsageMB()   const { auto* p = device(); return p ? p->getVRAMUsageMB() : 0; }
	bool        isInitialized() const { return gfxEngineIsInitialized(m_engine); }
	std::string lastError()     const { return gfxEngineGetLastError(m_engine); }

	// 現在のデバイス (vtable を直接使う場合)。switchAPI のたびに変わるので保持しないこと
	IBackendPlugin* device()       const { return gfxEngineGetDevice(m_engine); }
	// 本体 DLL のエンジンハンドル (C ABI を直接使う場合)
	GfxEngine*      handle()       const { return m_engine; }

	using SwapCallback = std::function<void(GraphicsAPI, GraphicsAPI)>;
	void setPreSwapCallback (SwapCallback cb) { m_preSwap  = std::move(cb); }
	void setPostSwapCallback(SwapCallback cb) { m_postSwap = std::move(cb); }

private:
	static void preSwapThunk(void* user, GraphicsAPI oldApi, GraphicsAPI newApi)
	{
		auto* self = static_cast<GraphicsEngine*>(user);
		if (self->m_preSwap) self->m_preSwap(oldApi, newApi);
	}
	static void postSwapThunk(void* user, GraphicsAPI oldApi, GraphicsAPI newApi)
	{
		auto* self = static_cast<GraphicsEngine*>(user);
		if (self->m_postSwap) self->m_postSwap(oldApi, newApi);
	}

	static void debugMessageThunk(void* user, GfxMessageSeverity severity, int32_t id, const char* text)
	{
		auto* self = static_cast<GraphicsEngine*>(user);
		if (self->m_debugMessage) self->m_debugMessage(severity, id, text);
	}

	GfxEngine*                 m_engine = nullptr;
	std::vector<RenderWindow*> m_windows;
	SwapCallback               m_preSwap, m_postSwap;
	std::function<void(GfxMessageSeverity, int32_t, const char*)> m_debugMessage;
};
