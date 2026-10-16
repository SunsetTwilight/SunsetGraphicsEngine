// ============================================================
//  GraphicsEngine.cpp  (RT/DS 分離版)
// ============================================================
#include "gfx/core/GraphicsEngine.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// 実行ファイルが存在するディレクトリを返す (取得できなければ ".")
static std::string executableDir()
{
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return fs::path(buf).parent_path().string();
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (_NSGetExecutablePath(buf.data(), &size) != 0)
		return ".";
	std::error_code ec;
	const fs::path p = fs::canonical(fs::path(buf.c_str()), ec);
	return ec ? std::string(".") : p.parent_path().string();
#else
	std::error_code ec;
	const fs::path p = fs::canonical("/proc/self/exe", ec);
	return ec ? std::string(".") : p.parent_path().string();
#endif
}

// ============================================================
//  RenderWindow
//  switchAPI に失敗した場合などスワップチェーンが無い状態でも落ちないようにガードする
// ============================================================
bool RenderWindow::acquireNextBuffer()
{
	return m_swapChain ? m_swapChain->acquireNextBuffer() : false;
}
IRenderTarget* RenderWindow::getRenderTarget()
{
	return m_swapChain ? m_swapChain->getRenderTarget() : nullptr;
}
void RenderWindow::present()
{
	if (m_swapChain) m_swapChain->present();
}
bool RenderWindow::resize(uint32_t w, uint32_t h)
{
	if (!m_swapChain) return false;
	if (!m_swapChain->resize(w, h)) return false;
	// 最小化 (0x0) はスワップチェーン側で無視されるため、有効なサイズだけ記録する
	if (w > 0 && h > 0) { m_desc.width = w; m_desc.height = h; }
	return true;
}
uint32_t    RenderWindow::width()  const { return m_swapChain ? m_swapChain->getWidth()  : 0; }
uint32_t    RenderWindow::height() const { return m_swapChain ? m_swapChain->getHeight() : 0; }
PixelFormat RenderWindow::format() const { return m_swapChain ? m_swapChain->getFormat() : PixelFormat::Unknown; }

// ============================================================
//  GraphicsEngine
// ============================================================
GraphicsEngine::GraphicsEngine()
{
	const std::string exeDir = executableDir();
	m_searchPaths.push_back(exeDir);
	// インストール配置 (<prefix>/bin/exe と <prefix>/lib/*.so) にも対応する
	m_searchPaths.push_back((fs::path(exeDir) / ".." / "lib").string());
}
GraphicsEngine::~GraphicsEngine() { shutdown(); }

void GraphicsEngine::addPluginSearchPath(const std::string& dir) { m_searchPaths.push_back(dir); }

bool GraphicsEngine::init(const DeviceConfig& cfg) {
	if (m_initialized) shutdown();
	m_config = cfg;
	GraphicsAPI api = cfg.api;
	if (api == GraphicsAPI::Auto) api = (
#if defined(_WIN32)
		GraphicsAPI::DirectX12
#else
		GraphicsAPI::Vulkan
#endif
	);
	if (!loadPlugin(api)) { std::cerr << "[Engine] " << m_loader.lastError() << "\n"; unloadPlugin(); return false; }
	GfxDeviceConfig pod = m_config.toPOD(); pod.api = api;
	if (!m_plugin->init(pod)) { unloadPlugin(); return false; }
	m_initialized = true;
	std::cout << "[Engine] " << m_plugin->getAdapterName()
			  << " / " << (api == GraphicsAPI::DirectX12 ? "DX12" : "Vulkan") << "\n";
	return true;
}

void GraphicsEngine::shutdown() {
	for (auto* w : m_windows) {
		if (w->m_swapChain)
		{
			w->m_swapChain->destroy();   // ラッパーごと DLL 内で解放される
			w->m_swapChain = nullptr;
		}
		delete w;
	}
	m_windows.clear();
	unloadPlugin();
	m_initialized = false;
}

// ---- ウィンドウ管理 ----
RenderWindow* GraphicsEngine::createWindow(const WindowDesc& desc) {
	if (!m_initialized || !m_plugin) return nullptr;
	auto pod = desc.toPOD();
	ISwapChain* sc = m_plugin->createSwapChain(pod);
	if (!sc) { std::cerr << "[Engine] createSwapChain failed (" << desc.name << ")\n"; return nullptr; }
	auto* win = new RenderWindow(sc, desc);
	m_windows.push_back(win);
	std::cout << "[Engine] Window \"" << desc.name << "\" " << desc.width << "x" << desc.height << "\n";
	return win;
}

void GraphicsEngine::destroyWindow(RenderWindow* win) {
	if (!win) return;
	auto it = std::find(m_windows.begin(), m_windows.end(), win);
	if (it == m_windows.end()) return;
	if (win->m_swapChain)
	{
		win->m_swapChain->destroy();   // ラッパーごと DLL 内で解放される
		win->m_swapChain = nullptr;
	}
	m_windows.erase(it);
	delete win;
}

// ---- RT / DS ----
IRenderTarget* GraphicsEngine::createRenderTarget(const RenderTargetDesc& desc) {
	return m_plugin ? m_plugin->createRenderTarget(desc) : nullptr;
}
IDepthStencil* GraphicsEngine::createDepthStencil(const DepthStencilDesc& desc) {
	return m_plugin ? m_plugin->createDepthStencil(desc) : nullptr;
}
void GraphicsEngine::destroyRenderTarget(IRenderTarget* rt)
{
	if (!rt || !m_plugin) return;
	rt->destroy();   // ラッパーごと DLL 内で解放される (EXE 側で delete しない)
}
void GraphicsEngine::destroyDepthStencil(IDepthStencil* ds)
{
	if (!ds || !m_plugin) return;
	ds->destroy();   // ラッパーごと DLL 内で解放される (EXE 側で delete しない)
}

// ---- コマンドバッファ管理 ----
ICommandBuffer* GraphicsEngine::allocCommandBuffer(CommandBufferType type) {
	return m_plugin ? m_plugin->allocCommandBuffer(type) : nullptr;
}

void GraphicsEngine::submitCommandBuffers(std::initializer_list<ICommandBuffer*> cbs) {
	if (!m_plugin) return;
	std::vector<ICommandBuffer*> vec(cbs);
	m_plugin->submitCommandBuffers(vec.data(), static_cast<uint32_t>(vec.size()));
}

// ---- GPU リソース ----
// バックエンド未ロード (init 前 / switchAPI 失敗後) は無効ハンドルを返し、破棄は何もしない
BufferHandle  GraphicsEngine::createBuffer (const BufferDesc&     d) { return m_plugin ? m_plugin->createBuffer(d)  : BufferHandle{};  }
TextureHandle GraphicsEngine::createTexture(const TextureDesc&    d) { return m_plugin ? m_plugin->createTexture(d) : TextureHandle{}; }
ShaderHandle  GraphicsEngine::compileShader(const ShaderDesc&     d) { return m_plugin ? m_plugin->compileShader(d) : ShaderHandle{};  }
SamplerHandle GraphicsEngine::createSampler(const GfxSamplerDesc& d) { return m_plugin ? m_plugin->createSampler(d) : SamplerHandle{}; }
void GraphicsEngine::destroyBuffer (BufferHandle  h) { if (m_plugin) m_plugin->destroyBuffer(h);  }
void GraphicsEngine::destroyTexture(TextureHandle h) { if (m_plugin) m_plugin->destroyTexture(h); }
void GraphicsEngine::destroyShader (ShaderHandle  h) { if (m_plugin) m_plugin->destroyShader(h);  }
void GraphicsEngine::destroySampler(SamplerHandle h) { if (m_plugin) m_plugin->destroySampler(h); }
PipelineHandle GraphicsEngine::createPipeline(const GfxPipelineDesc& d) { return m_plugin ? m_plugin->createPipeline(d) : PipelineHandle{}; }
void GraphicsEngine::destroyPipeline(PipelineHandle h) { if (m_plugin) m_plugin->destroyPipeline(h); }
PipelineHandle GraphicsEngine::createPipeline(const GfxGraphicsPipelineDesc& d) { return m_plugin ? m_plugin->createPipeline(d) : PipelineHandle{}; }
BindingLayoutHandle GraphicsEngine::createBindingLayout(const GfxBindingLayoutDesc& d) { return m_plugin ? m_plugin->createBindingLayout(d) : BindingLayoutHandle{}; }
void GraphicsEngine::destroyBindingLayout(BindingLayoutHandle h) { if (m_plugin) m_plugin->destroyBindingLayout(h); }
PipelineHandle GraphicsEngine::createPipeline(const GfxComputePipelineDesc& d) { return m_plugin ? m_plugin->createPipeline(d) : PipelineHandle{}; }
QueryPoolHandle GraphicsEngine::createQueryPool(const GfxQueryPoolDesc& d) { return m_plugin ? m_plugin->createQueryPool(d) : QueryPoolHandle{}; }
void GraphicsEngine::destroyQueryPool(QueryPoolHandle h) { if (m_plugin) m_plugin->destroyQueryPool(h); }
bool GraphicsEngine::readQueryResults(QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out) { return m_plugin && m_plugin->readQueryResults(h, first, count, out); }
uint64_t GraphicsEngine::getTimestampFrequency() const { return m_plugin ? m_plugin->getTimestampFrequency() : 0; }
TextureHandle GraphicsEngine::createTexture(const GfxTextureResourceDesc& d) { return m_plugin ? m_plugin->createTexture(d) : TextureHandle{}; }
ViewHandle GraphicsEngine::createView(const GfxViewDesc& d) { return m_plugin ? m_plugin->createView(d) : ViewHandle{}; }
void GraphicsEngine::destroyView(ViewHandle h) { if (m_plugin) m_plugin->destroyView(h); }
SamplerHandle GraphicsEngine::createSampler(const GfxSamplerStateDesc& d) { return m_plugin ? m_plugin->createSampler(d) : SamplerHandle{}; }
BLASHandle GraphicsEngine::createBLAS(const GfxBLASDesc& d) { return m_plugin ? m_plugin->createBLAS(d) : BLASHandle{}; }
void GraphicsEngine::destroyBLAS(BLASHandle h) { if (m_plugin) m_plugin->destroyBLAS(h); }
TLASHandle GraphicsEngine::createTLAS(const GfxTLASInstance* inst, uint32_t n) { return m_plugin ? m_plugin->createTLAS(inst, n) : TLASHandle{}; }
void GraphicsEngine::destroyTLAS(TLASHandle h) { if (m_plugin) m_plugin->destroyTLAS(h); }
void GraphicsEngine::updateTLAS(TLASHandle h, const GfxTLASInstance* inst, uint32_t n) { if (m_plugin) m_plugin->updateTLAS(h, inst, n); }
RTPipelineHandle GraphicsEngine::createRTPipeline(const GfxRTPipelineDesc& d) { return m_plugin ? m_plugin->createRTPipeline(d) : RTPipelineHandle{}; }
void GraphicsEngine::destroyRTPipeline(RTPipelineHandle h) { if (m_plugin) m_plugin->destroyRTPipeline(h); }
bool GraphicsEngine::updateBuffer(BufferHandle h, const void* data, uint64_t size)
{
	return m_plugin ? m_plugin->updateBuffer(h, data, size) : false;
}

void GraphicsEngine::bindTexture   (TextureHandle h, uint32_t s) { if (m_plugin) m_plugin->bindTexture(h, s); }
void GraphicsEngine::draw          (const DrawCmd& cmd)          { if (m_plugin) m_plugin->draw(cmd);          }
void GraphicsEngine::drawFullscreen()                            { if (m_plugin) m_plugin->drawFullscreen();    }

GraphicsAPI GraphicsEngine::currentAPI()  const { return m_plugin ? m_plugin->getAPI() : GraphicsAPI::Auto;      }
std::string GraphicsEngine::adapterName() const { return m_plugin ? m_plugin->getAdapterName() : std::string(); }
uint64_t    GraphicsEngine::vramUsageMB() const { return m_plugin ? m_plugin->getVRAMUsageMB() : 0;              }

// ---- ホットスワップ ----
bool GraphicsEngine::switchAPI(GraphicsAPI newAPI) {
	if (!m_initialized || !m_plugin) return false;
	if (newAPI == GraphicsAPI::Auto || m_plugin->getAPI() == newAPI) return true;

	const GraphicsAPI old = m_plugin->getAPI();

	// アプリはここで旧バックエンドのリソース (RT / DS / Buffer / Pipeline 等) を破棄する
	if (m_preSwap) m_preSwap(old, newAPI);

	// スワップチェーンは旧 DLL のオブジェクトなので、DLL をアンロードする前に破棄する
	for (auto* w : m_windows)
	{
		if (w->m_swapChain)
		{
			w->m_swapChain->destroy();
			w->m_swapChain = nullptr;
		}
	}
	unloadPlugin();

	auto fail = [this](const char* what) {
		std::cerr << "[Engine] switchAPI failed: " << what << " " << m_loader.lastError() << "\n";
		unloadPlugin();
		m_initialized = false;
		return false;
	};

	if (!loadPlugin(newAPI))
		return fail("loadPlugin");
	GfxDeviceConfig pod = m_config.toPOD(); pod.api = newAPI;
	if (!m_plugin->init(pod))
		return fail("init");
	m_config.api = newAPI;

	// ウィンドウのスワップチェーンを新しいバックエンドで作り直す (直近のサイズを使う)
	for (auto* w : m_windows)
	{
		w->m_swapChain = m_plugin->createSwapChain(w->m_desc.toPOD());
		if (!w->m_swapChain)
			std::cerr << "[Engine] switchAPI: createSwapChain failed (" << w->m_desc.name << ")\n";
	}

	if (m_postSwap) m_postSwap(old, newAPI);
	return true;
}

// ---- 内部 ----
bool GraphicsEngine::loadPlugin(GraphicsAPI api) {
	std::string base = resolveDllPath(dllBaseName(api));
	if (!m_loader.load(base)) return false;
	m_plugin = m_loader.createPlugin();
	return m_plugin != nullptr;
}
void GraphicsEngine::unloadPlugin() {
	if (m_plugin) { m_loader.destroyPlugin(m_plugin); m_plugin = nullptr; }
	m_loader.unload();
}
std::string GraphicsEngine::dllBaseName(GraphicsAPI api) const {
	return api == GraphicsAPI::DirectX12 ? "gfx_dx12" : "gfx_vulkan";
}
// DLL の実フルパス(拡張子なし)を m_searchPaths から探して返す
// 見つからなければ base をそのまま返す (LoadLibrary に任せる)
std::string GraphicsEngine::resolveDllPath(const std::string& base) const
{
#if defined(_WIN32)
	constexpr std::string_view ext = ".dll";
#elif defined(__APPLE__)
	constexpr std::string_view ext = ".dylib";
#else
	constexpr std::string_view ext = ".so";
#endif

	for (const auto& dir : m_searchPaths)
	{
		fs::path candidate = fs::path(dir) / (base + std::string(ext));

		std::error_code ec;
		if (fs::exists(candidate, ec))
			return (fs::path(dir) / base).string();
	}

	return base;
}
