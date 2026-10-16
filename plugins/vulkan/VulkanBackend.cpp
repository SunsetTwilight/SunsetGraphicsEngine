// ============================================================
//  plugins/vulkan/VulkanBackend.cpp  (RT/DS 分離版)
//
//  クラス構成:
//    VkRenderTargetImpl  … VkImage (カラー RT) + VkImageView
//    VkDepthStencilImpl  … VkImage (DS) + VkImageView
//    VkSwapChainImpl     … VkSwapchainKHR + バックバッファ用 VkRenderTargetImpl
//                          ★ DSV を持たない
//    VkDeviceImpl        … VkDevice / キュー / リソース管理
// ============================================================

#include "GfxBackendABI.h"
#include <vulkan/vulkan.h>
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif
#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

// ============================================================
//  前方宣言
// ============================================================
class VkDeviceImpl;

// ============================================================
//  ヘルパー
// ============================================================
static VkFormat toVkFormat(PixelFormat fmt) {
	switch (fmt) {
		case PixelFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
		case PixelFormat::BGRA8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
		case PixelFormat::RGBA16_FLOAT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
		case PixelFormat::R32_FLOAT:         return VK_FORMAT_R32_SFLOAT;
		case PixelFormat::D32_FLOAT:         return VK_FORMAT_D32_SFLOAT;
		case PixelFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		case PixelFormat::D16_UNORM:         return VK_FORMAT_D16_UNORM;
		default:                             return VK_FORMAT_UNDEFINED;
	}
}
static bool isDepthFormat(PixelFormat fmt) {
	return fmt == PixelFormat::D32_FLOAT ||
		   fmt == PixelFormat::D24_UNORM_S8_UINT ||
		   fmt == PixelFormat::D16_UNORM;
}
static VkImageAspectFlags aspectOf(PixelFormat fmt) {
	if (fmt == PixelFormat::D24_UNORM_S8_UINT)
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if (isDepthFormat(fmt)) return VK_IMAGE_ASPECT_DEPTH_BIT;
	return VK_IMAGE_ASPECT_COLOR_BIT;
}

// ============================================================
//  VkRenderTargetImpl
//  カラー RT 1 枚。SwapChain バックバッファ用にも独立 RT 用にも使える。
// ============================================================
class VkRenderTargetImpl {
public:
	// 独立オフスクリーン RT として作成
	bool create(VkDeviceImpl* dev, const GfxRenderTargetDesc* desc);
	// SwapChain バックバッファから作成 (外部 VkImage を受け取る)
	bool createFromSwapBuffer(VkDeviceImpl* dev, VkImage image,
							  VkFormat vkFmt, PixelFormat fmt,
							  uint32_t w, uint32_t h);
	void destroy();
	bool resize(uint32_t w, uint32_t h);

	uint32_t    getWidth()  const { return m_width;  }
	uint32_t    getHeight() const { return m_height; }
	PixelFormat getFormat() const { return m_format; }
	TextureHandle asTexture();

	VkImage     image()     const { return m_image;     }
	VkImageView view()      const { return m_view;      }
	VkImageLayout layout()  const { return m_layout;    }
	void setLayout(VkImageLayout l) { m_layout = l; }
	bool ownImage() const { return m_ownImage; }

	static const IRenderTargetVTable* getVTable();

private:
	VkDeviceImpl*  m_dev       = nullptr;
	VkImage        m_image     = VK_NULL_HANDLE;
	VkImageView    m_view      = VK_NULL_HANDLE;
	VkDeviceMemory m_memory    = VK_NULL_HANDLE;
	VkImageLayout  m_layout    = VK_IMAGE_LAYOUT_UNDEFINED;
	bool           m_ownImage  = false;

	// SRV 用 (allowUAV=true または asTexture() 呼び出し時に作成)
	VkImageView    m_srvView   = VK_NULL_HANDLE;

	uint32_t    m_width  = 0, m_height = 0;
	PixelFormat m_format = PixelFormat::RGBA8_UNORM;
	GfxRenderTargetDesc m_desc = {};

	bool allocImage();
	void freeImage();
	bool makeView(VkImage img, VkFormat fmt, VkImageView& out);
};

// ============================================================
//  VkDepthStencilImpl
//  DS 1 枚。SwapChain とは完全に独立。
// ============================================================
class VkDepthStencilImpl {
public:
	bool create(VkDeviceImpl* dev, const GfxDepthStencilDesc* desc);
	void destroy();
	bool resize(uint32_t w, uint32_t h);

	uint32_t    getWidth()  const { return m_width;  }
	uint32_t    getHeight() const { return m_height; }
	PixelFormat getFormat() const { return m_format; }
	TextureHandle asTexture();

	VkImage     image()  const { return m_image; }
	VkImageView view()   const { return m_view;  }
	VkImageLayout layout() const { return m_layout; }
	void setLayout(VkImageLayout l) { m_layout = l; }

	static const IDepthStencilVTable* getVTable();

private:
	VkDeviceImpl*  m_dev    = nullptr;
	VkImage        m_image  = VK_NULL_HANDLE;
	VkImageView    m_view   = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	VkImageLayout  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint32_t    m_width = 0, m_height = 0;
	PixelFormat m_format = PixelFormat::D32_FLOAT;
	GfxDepthStencilDesc m_desc = {};

	bool allocImage();
	void freeImage();
};

// ============================================================
//  VkCommandBufferImpl
//  アプリが複数保持できるコマンドバッファ。
//  Direct  … Primary   コマンドバッファ。vkQueueSubmit に渡せる。
//  Bundle  … Secondary コマンドバッファ。Primary から vkCmdExecuteCommands で呼ぶ。
//
//  各インスタンスが専用の VkCommandPool を持つため、
//  同一フレーム内での並列 begin/end 記録が安全に行える。
// ============================================================
class VkCommandBufferImpl {
public:
	bool create(VkDeviceImpl* dev, CommandBufferType type);
	void begin();
	void end();
	void executeBundle(VkCommandBufferImpl* secondary);
	void release();  // DLL 内部で delete する

	void setRenderTargets(uint32_t rtCount, IRenderTarget* const* rts,
						  IDepthStencil* ds, bool clearRT, bool clearDS);
	void bindShader    (ShaderHandle  h);
	void bindTexture   (TextureHandle h, uint32_t slot);
	void draw          (const GfxDrawCmd* cmd);
	void drawFullscreen();

	CommandBufferType getType()   const { return m_type; }
	VkCommandBuffer   cmdBuf()    const { return m_cmdBuf; }

	static const ICommandBufferVTable* getVTable();

private:
	VkDeviceImpl*     m_dev     = nullptr;
	VkCommandPool     m_pool    = VK_NULL_HANDLE;
	VkCommandBuffer   m_cmdBuf  = VK_NULL_HANDLE;
	CommandBufferType m_type    = CommandBufferType::Direct;

	// Dynamic Rendering 用の添付情報（begin 〜 end の区間で有効）
	bool m_renderingActive = false;
};

// ============================================================
//  VkSwapChainImpl
//  VkSwapchainKHR + フレームごとの VkRenderTargetImpl
//  ★ DepthStencil を持たない
// ============================================================
class VkSwapChainImpl {
public:
	bool create(VkDeviceImpl* dev, const GfxSwapChainDesc* desc);
	void destroy();
	bool resize(uint32_t w, uint32_t h);

	bool           acquireNextBuffer();
	IRenderTarget* getRenderTarget();
	void           present();

	uint32_t    getWidth()  const { return m_extent.width;  }
	uint32_t    getHeight() const { return m_extent.height; }
	PixelFormat getFormat() const { return m_format; }

	static const ISwapChainVTable* getVTable();

private:
	// bufferCount に合わせて動的に変わる。最大 MAX_FRAMES まで対応。
	static constexpr uint32_t MAX_FRAMES = 3;

	VkDeviceImpl*         m_dev       = nullptr;
	VkSurfaceKHR          m_surface   = VK_NULL_HANDLE;
	VkSwapchainKHR        m_swapchain = VK_NULL_HANDLE;

	// バックバッファ RT (DSV は持たない)
	VkRenderTargetImpl*   m_backRTs[MAX_FRAMES] = {};
	uint32_t              m_imageCount = 0;

	// フレーム同期 (スワップチェーン専用)
	// m_framesInFlight は bufferCount から決まる (2 or 3)
	uint32_t        m_framesInFlight = 2;
	VkCommandPool   m_cmdPool = VK_NULL_HANDLE;
	VkCommandBuffer m_cmdBufs[MAX_FRAMES] = {};
	VkSemaphore     m_imageAvail[MAX_FRAMES] = {};
	VkSemaphore     m_renderDone[MAX_FRAMES] = {};
	VkFence         m_inFlight  [MAX_FRAMES] = {};
	uint32_t        m_frameIdx = 0;
	uint32_t        m_imageIdx = 0;

	VkFormat    m_vkFormat = VK_FORMAT_UNDEFINED;
	PixelFormat m_format   = PixelFormat::BGRA8_UNORM;
	VkExtent2D  m_extent   = {};

	// 現フレームの IRenderTarget* キャッシュ
	IRenderTarget m_currentRT;

	bool buildSwapchain(uint32_t w, uint32_t h);
	void destroySwapchain();
	bool createSyncObjects();
};

// ============================================================
//  VkDeviceImpl
// ============================================================
struct VkBufRes  { VkBuffer vk = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize sz = 0; };
struct VkTexRes  { VkImage vk = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; };
struct VkShadRes { VkShaderModule vk = VK_NULL_HANDLE; ShaderStage stage = ShaderStage::Vertex; };

class VkCommandBufferImpl; // 前方宣言

class VkDeviceImpl {
public:
	bool init(const GfxDeviceConfig* cfg);
	void shutdown();

	ISwapChain*    createSwapChain   (const GfxSwapChainDesc*    desc);
	IRenderTarget* createRenderTarget(const GfxRenderTargetDesc* desc);
	IDepthStencil* createDepthStencil(const GfxDepthStencilDesc* desc);

	// 外部コマンドバッファ管理 (複数作成・並列記録対応)
	ICommandBuffer* allocCommandBuffer(CommandBufferType type);
	void            submitCommandBuffers(ICommandBuffer* const* cbs, uint32_t count);

	// 互換用 (内部コマンドバッファへのショートカット)
	void beginCommands();
	void endCommands();
	void setRenderTargets(uint32_t rtCount, IRenderTarget* const* rts,
						  IDepthStencil* ds, bool clearRT, bool clearDS);

	BufferHandle  createBuffer (const GfxBufferDesc*  d);
	TextureHandle createTexture(const GfxTextureDesc* d);
	ShaderHandle  compileShader(const GfxShaderDesc*  d);
	void destroyBuffer (BufferHandle  h);
	void destroyTexture(TextureHandle h);
	void destroyShader (ShaderHandle  h);
	void bindShader    (ShaderHandle  h);
	void bindTexture   (TextureHandle h, uint32_t slot);
	void draw          (const GfxDrawCmd* cmd);
	void drawFullscreen();

	GraphicsAPI getAPI()         const { return GraphicsAPI::Vulkan; }
	const char* getAdapterName() const { return m_adapterName; }
	uint64_t    getVRAMUsageMB() const;

	static const GfxDeviceVTable* getVTable();

	// 内部公開
	VkInstance       instance()      { return m_instance;      }
	VkPhysicalDevice physDevice()    { return m_physDevice;    }
	VkDevice         device()        { return m_device;        }
	VkQueue          graphicsQueue() { return m_graphicsQueue; }
	VkQueue          presentQueue()  { return m_presentQueue;  }
	uint32_t         graphicsFamily(){ return m_graphicsFamily;}

	bool     allocMem(VkMemoryRequirements req, VkMemoryPropertyFlags props, VkDeviceMemory& out);
	bool     makeImageView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView& out);
	uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags props);

	// VkCommandBufferImpl から呼ばれる — 外部コマンドバッファへ draw を記録
	void drawOnCmdBuf(VkCommandBuffer cb, const GfxDrawCmd* cmd);

	// 現在記録中のコマンドバッファ (SwapChainImpl が beginFrame で設定)
	VkCommandBuffer currentCmdBuf = VK_NULL_HANDLE;

private:
	VkInstance       m_instance      = VK_NULL_HANDLE;
	VkPhysicalDevice m_physDevice    = VK_NULL_HANDLE;
	VkDevice         m_device        = VK_NULL_HANDLE;
	VkQueue          m_graphicsQueue = VK_NULL_HANDLE;
	VkQueue          m_presentQueue  = VK_NULL_HANDLE;
	uint32_t         m_graphicsFamily = 0;

	// デバイスレベルのコマンドプール (リソース転送等に使用)
	VkCommandPool    m_cmdPool  = VK_NULL_HANDLE;
	VkCommandBuffer  m_cmdBuf   = VK_NULL_HANDLE;

	std::unordered_map<uint64_t, VkBufRes>  m_buffers;
	std::unordered_map<uint64_t, VkTexRes>  m_textures;
	std::unordered_map<uint64_t, VkShadRes> m_shaders;
	uint64_t m_nextId = 1;

	char m_adapterName[256] = {};

	bool createInstance(bool validation);
	bool selectPhysicalDevice();
	bool createLogicalDevice();
	bool createCommandPool();

	friend class VkSwapChainImpl;
	friend class VkRenderTargetImpl;
	friend class VkDepthStencilImpl;

#ifdef _DEBUG
	VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
#endif
};

// ============================================================
//  vtable 定義
// ============================================================

// --- IRenderTarget ---
static const IRenderTargetVTable k_VkRTVTable = {
	[](void* s) { auto* i=static_cast<VkRenderTargetImpl*>(s); i->destroy(); delete i; },
	[](void* s, uint32_t w, uint32_t h)->bool { return static_cast<VkRenderTargetImpl*>(s)->resize(w,h); },
	[](const void* s)->uint32_t    { return static_cast<const VkRenderTargetImpl*>(s)->getWidth();  },
	[](const void* s)->uint32_t    { return static_cast<const VkRenderTargetImpl*>(s)->getHeight(); },
	[](const void* s)->PixelFormat { return static_cast<const VkRenderTargetImpl*>(s)->getFormat(); },
	[](void* s)->TextureHandle     { return static_cast<VkRenderTargetImpl*>(s)->asTexture(); },
};
const IRenderTargetVTable* VkRenderTargetImpl::getVTable() { return &k_VkRTVTable; }

// --- IDepthStencil ---
static const IDepthStencilVTable k_VkDSVTable = {
	[](void* s) { auto* i=static_cast<VkDepthStencilImpl*>(s); i->destroy(); delete i; },
	[](void* s, uint32_t w, uint32_t h)->bool { return static_cast<VkDepthStencilImpl*>(s)->resize(w,h); },
	[](const void* s)->uint32_t    { return static_cast<const VkDepthStencilImpl*>(s)->getWidth();  },
	[](const void* s)->uint32_t    { return static_cast<const VkDepthStencilImpl*>(s)->getHeight(); },
	[](const void* s)->PixelFormat { return static_cast<const VkDepthStencilImpl*>(s)->getFormat(); },
	[](void* s)->TextureHandle     { return static_cast<VkDepthStencilImpl*>(s)->asTexture(); },
};
const IDepthStencilVTable* VkDepthStencilImpl::getVTable() { return &k_VkDSVTable; }

// --- ISwapChain ---
static const ISwapChainVTable k_VkSCVTable = {
	[](void* s) { auto* i=static_cast<VkSwapChainImpl*>(s); i->destroy(); delete i; },
	[](void* s, uint32_t w, uint32_t h)->bool { return static_cast<VkSwapChainImpl*>(s)->resize(w,h); },
	[](void* s)->bool          { return static_cast<VkSwapChainImpl*>(s)->acquireNextBuffer(); },
	[](void* s)->IRenderTarget*{ return static_cast<VkSwapChainImpl*>(s)->getRenderTarget(); },
	[](void* s)                { static_cast<VkSwapChainImpl*>(s)->present(); },
	[](const void* s)->uint32_t    { return static_cast<const VkSwapChainImpl*>(s)->getWidth();  },
	[](const void* s)->uint32_t    { return static_cast<const VkSwapChainImpl*>(s)->getHeight(); },
	[](const void* s)->PixelFormat { return static_cast<const VkSwapChainImpl*>(s)->getFormat(); },
};
const ISwapChainVTable* VkSwapChainImpl::getVTable() { return &k_VkSCVTable; }

// --- ICommandBuffer (Vulkan) ---
// VkCommandBufferImpl の前方宣言はクラス定義の前に移動済み
static const ICommandBufferVTable k_VkCmdVTable = {
	[](void* s) { static_cast<VkCommandBufferImpl*>(s)->begin(); },
	[](void* s) { static_cast<VkCommandBufferImpl*>(s)->end(); },
	[](void* s, void* p) { static_cast<VkCommandBufferImpl*>(s)->executeBundle(
		static_cast<VkCommandBufferImpl*>(p)); },
	[](void* s) { static_cast<VkCommandBufferImpl*>(s)->release(); },
	[](void* s, uint32_t n, IRenderTarget* const* rts, IDepthStencil* ds, bool cr, bool cd) {
		static_cast<VkCommandBufferImpl*>(s)->setRenderTargets(n, rts, ds, cr, cd); },
	[](void* s, ShaderHandle h)                 { static_cast<VkCommandBufferImpl*>(s)->bindShader(h); },
	[](void* s, TextureHandle h, uint32_t slot) { static_cast<VkCommandBufferImpl*>(s)->bindTexture(h, slot); },
	[](void* s, const GfxDrawCmd* cmd)          { static_cast<VkCommandBufferImpl*>(s)->draw(cmd); },
	[](void* s)                                 { static_cast<VkCommandBufferImpl*>(s)->drawFullscreen(); },
	[](const void* s)->CommandBufferType { return static_cast<const VkCommandBufferImpl*>(s)->getType(); },
};
const ICommandBufferVTable* VkCommandBufferImpl::getVTable() { return &k_VkCmdVTable; }

// --- Device (ABI 順: init/shutdown/createSwapChain/createRT/createDS/
//             allocCB/submitCBs/createBuffer/createTexture/compileShader/
//             destroyBuffer/destroyTexture/destroyShader/
//             bindShader/bindTexture/draw/drawFullscreen/
//             getAPI/getAdapterName/getVRAMUsageMB) ---
static const GfxDeviceVTable k_VkDevVTable = {
	[](void* s, const GfxDeviceConfig* c)->bool  { return static_cast<VkDeviceImpl*>(s)->init(c); },
	[](void* s) { static_cast<VkDeviceImpl*>(s)->shutdown(); },
	[](void* s, const GfxSwapChainDesc* d)->ISwapChain*      { return static_cast<VkDeviceImpl*>(s)->createSwapChain(d);    },
	[](void* s, const GfxRenderTargetDesc* d)->IRenderTarget* { return static_cast<VkDeviceImpl*>(s)->createRenderTarget(d); },
	[](void* s, const GfxDepthStencilDesc* d)->IDepthStencil* { return static_cast<VkDeviceImpl*>(s)->createDepthStencil(d); },
	// ---- コマンドバッファ管理 ----
	[](void* s, CommandBufferType t)->ICommandBuffer* { return static_cast<VkDeviceImpl*>(s)->allocCommandBuffer(t); },
	[](void* s, ICommandBuffer* const* cbs, uint32_t n) { static_cast<VkDeviceImpl*>(s)->submitCommandBuffers(cbs, n); },
	// ---- GPU リソース ----
	[](void* s, const GfxBufferDesc* d)->BufferHandle   { return static_cast<VkDeviceImpl*>(s)->createBuffer(d);  },
	[](void* s, const GfxTextureDesc* d)->TextureHandle { return static_cast<VkDeviceImpl*>(s)->createTexture(d); },
	[](void* s, const GfxShaderDesc* d)->ShaderHandle   { return static_cast<VkDeviceImpl*>(s)->compileShader(d); },
	[](void* s, BufferHandle h)  { static_cast<VkDeviceImpl*>(s)->destroyBuffer(h);  },
	[](void* s, TextureHandle h) { static_cast<VkDeviceImpl*>(s)->destroyTexture(h); },
	[](void* s, ShaderHandle h)  { static_cast<VkDeviceImpl*>(s)->destroyShader(h);  },
	// ---- 描画 ----
	[](void* s, ShaderHandle h)                    { static_cast<VkDeviceImpl*>(s)->bindShader(h);       },
	[](void* s, TextureHandle h, uint32_t slot)    { static_cast<VkDeviceImpl*>(s)->bindTexture(h,slot); },
	[](void* s, const GfxDrawCmd* c)               { static_cast<VkDeviceImpl*>(s)->draw(c);             },
	[](void* s)                                    { static_cast<VkDeviceImpl*>(s)->drawFullscreen();     },
	// ---- クエリ ----
	[](const void* s)->GraphicsAPI { return static_cast<const VkDeviceImpl*>(s)->getAPI(); },
	[](const void* s)->const char* { return static_cast<const VkDeviceImpl*>(s)->getAdapterName(); },
	[](const void* s)->uint64_t    { return static_cast<const VkDeviceImpl*>(s)->getVRAMUsageMB(); },
};
const GfxDeviceVTable* VkDeviceImpl::getVTable() { return &k_VkDevVTable; }

GFX_PLUGIN_EXPORT_FUNCTIONS(VkDeviceImpl)

// ============================================================
//  VkCommandBufferImpl 実装
// ============================================================
bool VkCommandBufferImpl::create(VkDeviceImpl* dev, CommandBufferType type) {
	m_dev  = dev;
	m_type = type;

	// コマンドバッファごとに専用プールを持つ
	// → リセット時に他のバッファに影響しない
	VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pi.queueFamilyIndex = dev->graphicsFamily();
	pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(dev->device(), &pi, nullptr, &m_pool) != VK_SUCCESS)
		return false;

	VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool        = m_pool;
	ai.level              = (type == CommandBufferType::Bundle)
								? VK_COMMAND_BUFFER_LEVEL_SECONDARY
								: VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	return vkAllocateCommandBuffers(dev->device(), &ai, &m_cmdBuf) == VK_SUCCESS;
}

void VkCommandBufferImpl::begin() {
	vkResetCommandBuffer(m_cmdBuf, 0);

	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	// Secondary (Bundle) はインヘリタンス情報が必要
	VkCommandBufferInheritanceInfo inh{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
	if (m_type == CommandBufferType::Bundle) {
		// Dynamic Rendering を使うため renderPass は VK_NULL_HANDLE のまま
		inh.renderPass  = VK_NULL_HANDLE;
		inh.framebuffer = VK_NULL_HANDLE;
		bi.flags       |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
		bi.pInheritanceInfo = &inh;
	}
	vkBeginCommandBuffer(m_cmdBuf, &bi);
	m_renderingActive = false;
}

void VkCommandBufferImpl::end() {
	// 記録中の Dynamic Rendering を閉じる
	if (m_renderingActive) {
		vkCmdEndRendering(m_cmdBuf);
		m_renderingActive = false;
	}
	vkEndCommandBuffer(m_cmdBuf);
}

void VkCommandBufferImpl::executeBundle(VkCommandBufferImpl* secondary) {
	if (secondary && secondary->m_type == CommandBufferType::Bundle)
		vkCmdExecuteCommands(m_cmdBuf, 1, &secondary->m_cmdBuf);
}

void VkCommandBufferImpl::release() {
	if (m_pool) {
		vkDestroyCommandPool(m_dev->device(), m_pool, nullptr);
		m_pool   = VK_NULL_HANDLE;
		m_cmdBuf = VK_NULL_HANDLE;
	}
	delete this;
}

void VkCommandBufferImpl::setRenderTargets(uint32_t rtCount,
											IRenderTarget* const* rts,
											IDepthStencil* ds,
											bool clearRT, bool clearDS) {
	// 前のレンダリングパスを閉じる
	if (m_renderingActive) {
		vkCmdEndRendering(m_cmdBuf);
		m_renderingActive = false;
	}

	// カラーアタッチメント
	std::vector<VkRenderingAttachmentInfo> colorAtts(rtCount);
	for (uint32_t i = 0; i < rtCount; ++i) {
		auto* rt = static_cast<VkRenderTargetImpl*>(rts[i]->self);

		// Undefined → ColorAttachment バリア
		VkImageMemoryBarrier2 bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		bar.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
		bar.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		bar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		bar.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
		bar.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		bar.image         = rt->image();
		bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &bar;
		vkCmdPipelineBarrier2(m_cmdBuf, &dep);

		auto& att  = colorAtts[i];
		att  = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		att.imageView   = rt->view();
		att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att.loadOp      = clearRT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		att.clearValue.color = { 0.f, 0.f, 0.f, 1.f };
	}

	// デプスアタッチメント
	VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	bool hasDS = (ds != nullptr);
	if (hasDS) {
		auto* d = static_cast<VkDepthStencilImpl*>(ds->self);
		depthAtt.imageView   = d->view();
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp      = clearDS ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAtt.clearValue.depthStencil = { 1.f, 0 };
	}

	// レンダリング開始
	VkExtent2D extent = (rtCount > 0)
		? VkExtent2D{ rts[0]->getWidth(), rts[0]->getHeight() }
		: VkExtent2D{ 1, 1 };

	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea.extent    = extent;
	ri.layerCount           = 1;
	ri.colorAttachmentCount = rtCount;
	ri.pColorAttachments    = colorAtts.empty() ? nullptr : colorAtts.data();
	ri.pDepthAttachment     = hasDS ? &depthAtt : nullptr;

	vkCmdBeginRendering(m_cmdBuf, &ri);
	m_renderingActive = true;

	// ビューポート / シザー
	VkViewport vp{ 0, 0, (float)extent.width, (float)extent.height, 0.f, 1.f };
	VkRect2D   sc{ {0,0}, extent };
	vkCmdSetViewport(m_cmdBuf, 0, 1, &vp);
	vkCmdSetScissor (m_cmdBuf, 0, 1, &sc);
}

void VkCommandBufferImpl::bindShader(ShaderHandle /*h*/) {
	// パイプラインバインドは別途実装
}

void VkCommandBufferImpl::bindTexture(TextureHandle /*h*/, uint32_t /*slot*/) {
	// ディスクリプタセット更新は別途実装
}

void VkCommandBufferImpl::draw(const GfxDrawCmd* cmd) {
	// デバイス経由でバッファを取得して記録
	m_dev->drawOnCmdBuf(m_cmdBuf, cmd);
}

void VkCommandBufferImpl::drawFullscreen() {
	vkCmdDraw(m_cmdBuf, 3, 1, 0, 0);
}

// ============================================================
//  VkDeviceImpl: allocCommandBuffer / submitCommandBuffers
// ============================================================
ICommandBuffer* VkDeviceImpl::allocCommandBuffer(CommandBufferType type) {
	auto* impl = new VkCommandBufferImpl();
	if (!impl->create(this, type)) { delete impl; return nullptr; }

	auto* cb     = new ICommandBuffer();
	cb->vtable   = VkCommandBufferImpl::getVTable();
	cb->self     = impl;
	return cb;
}

void VkDeviceImpl::submitCommandBuffers(ICommandBuffer* const* cbs, uint32_t count) {
	// Primary コマンドバッファのみをサブミット
	std::vector<VkCommandBuffer> bufs;
	bufs.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		auto* impl = static_cast<VkCommandBufferImpl*>(cbs[i]->self);
		if (impl->getType() == CommandBufferType::Direct)
			bufs.push_back(impl->cmdBuf());
	}
	if (bufs.empty()) return;

	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = static_cast<uint32_t>(bufs.size());
	si.pCommandBuffers    = bufs.data();
	vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
}

// ============================================================
//  VkDeviceImpl 実装
// ============================================================
bool VkDeviceImpl::init(const GfxDeviceConfig* cfg) {
	return createInstance(cfg->validation)
		&& selectPhysicalDevice()
		&& createLogicalDevice()
		&& createCommandPool();
}

bool VkDeviceImpl::createInstance(bool validation) {
	VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	ai.apiVersion = VK_API_VERSION_1_3;

	std::vector<const char*> exts = { VK_KHR_SURFACE_EXTENSION_NAME };
#if defined(_WIN32)
	exts.push_back("VK_KHR_win32_surface");
#elif defined(__APPLE__)
	exts.push_back("VK_EXT_metal_surface");
	exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#else
	exts.push_back("VK_KHR_xcb_surface");
#endif
	std::vector<const char*> layers;
	if (validation) { layers.push_back("VK_LAYER_KHRONOS_validation"); exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

	VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ci.pApplicationInfo        = &ai;
	ci.enabledExtensionCount   = (uint32_t)exts.size();
	ci.ppEnabledExtensionNames = exts.data();
	ci.enabledLayerCount       = (uint32_t)layers.size();
	ci.ppEnabledLayerNames     = layers.data();
#ifdef __APPLE__
	ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
	return vkCreateInstance(&ci, nullptr, &m_instance) == VK_SUCCESS;
}

bool VkDeviceImpl::selectPhysicalDevice() {
	uint32_t cnt = 0; vkEnumeratePhysicalDevices(m_instance, &cnt, nullptr);
	if (!cnt) return false;
	std::vector<VkPhysicalDevice> devs(cnt);
	vkEnumeratePhysicalDevices(m_instance, &cnt, devs.data());
	uint64_t best = 0;
	for (auto dev : devs) {
		VkPhysicalDeviceMemoryProperties mem;
		vkGetPhysicalDeviceMemoryProperties(dev, &mem);
		for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
			if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mem.memoryHeaps[i].size > best) {
				best = mem.memoryHeaps[i].size; m_physDevice = dev;
			}
		}
	}
	if (!m_physDevice) return false;
	VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(m_physDevice, &props);
	strncpy(m_adapterName, props.deviceName, 255);
	return true;
}

bool VkDeviceImpl::createLogicalDevice() {
	uint32_t qcnt = 0; vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &qcnt, nullptr);
	std::vector<VkQueueFamilyProperties> qp(qcnt);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &qcnt, qp.data());
	for (uint32_t i = 0; i < qcnt; ++i)
		if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { m_graphicsFamily = i; break; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = m_graphicsFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;

	VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	f2.pNext = &f13; f2.features.samplerAnisotropy = VK_TRUE;

	const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = exts;

	if (vkCreateDevice(m_physDevice, &dci, nullptr, &m_device) != VK_SUCCESS) return false;
	vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
	m_presentQueue = m_graphicsQueue;
	return true;
}

bool VkDeviceImpl::createCommandPool() {
	VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	ci.queueFamilyIndex = m_graphicsFamily;
	ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(m_device, &ci, nullptr, &m_cmdPool) != VK_SUCCESS) return false;
	VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = m_cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
	return vkAllocateCommandBuffers(m_device, &ai, &m_cmdBuf) == VK_SUCCESS;
}

ISwapChain* VkDeviceImpl::createSwapChain(const GfxSwapChainDesc* desc) {
	auto* impl = new VkSwapChainImpl();
	if (!impl->create(this, desc)) { delete impl; return nullptr; }
	auto* sc = new ISwapChain(); sc->vtable = VkSwapChainImpl::getVTable(); sc->self = impl;
	return sc;
}

IRenderTarget* VkDeviceImpl::createRenderTarget(const GfxRenderTargetDesc* desc) {
	auto* impl = new VkRenderTargetImpl();
	if (!impl->create(this, desc)) { delete impl; return nullptr; }
	auto* rt = new IRenderTarget(); rt->vtable = VkRenderTargetImpl::getVTable(); rt->self = impl;
	return rt;
}

IDepthStencil* VkDeviceImpl::createDepthStencil(const GfxDepthStencilDesc* desc) {
	auto* impl = new VkDepthStencilImpl();
	if (!impl->create(this, desc)) { delete impl; return nullptr; }
	auto* ds = new IDepthStencil(); ds->vtable = VkDepthStencilImpl::getVTable(); ds->self = impl;
	return ds;
}

void VkDeviceImpl::beginCommands() {
	vkResetCommandBuffer(m_cmdBuf, 0);
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(m_cmdBuf, &bi);
	currentCmdBuf = m_cmdBuf;
}

void VkDeviceImpl::endCommands() {
	vkEndCommandBuffer(m_cmdBuf);
	currentCmdBuf = VK_NULL_HANDLE;
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1; si.pCommandBuffers = &m_cmdBuf;
	vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_graphicsQueue);
}

void VkDeviceImpl::setRenderTargets(uint32_t rtCount, IRenderTarget* const* rts,
									 IDepthStencil* ds, bool clearRT, bool clearDS) {
	VkCommandBuffer cmd = currentCmdBuf;
	if (!cmd) return;

	// カラー RT のバリア (UNDEFINED/ShaderRead → ColorAttachment)
	std::vector<VkRenderingAttachmentInfo> colorAtts(rtCount);
	for (uint32_t i = 0; i < rtCount; ++i) {
		auto* rt = static_cast<VkRenderTargetImpl*>(rts[i]->self);

		// レイアウト遷移バリア
		if (rt->layout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
			VkImageMemoryBarrier2 bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			bar.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			bar.srcAccessMask = VK_ACCESS_2_NONE;
			bar.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			bar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			bar.oldLayout     = rt->layout();
			bar.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			bar.image         = rt->image();
			bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1 };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &bar;
			vkCmdPipelineBarrier2(cmd, &dep);
			rt->setLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		}

		colorAtts[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		colorAtts[i].imageView   = rt->view();
		colorAtts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtts[i].loadOp      = clearRT ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		colorAtts[i].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAtts[i].clearValue.color = { 0,0,0,1 };
	}

	// DS バリア
	VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	if (ds) {
		auto* d = static_cast<VkDepthStencilImpl*>(ds->self);
		if (d->layout() != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
			VkImageMemoryBarrier2 bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			bar.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			bar.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
			bar.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			bar.oldLayout     = d->layout();
			bar.newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			bar.image         = d->image();
			bar.subresourceRange = { aspectOf(d->getFormat()),0,1,0,1 };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &bar;
			vkCmdPipelineBarrier2(cmd, &dep);
			d->setLayout(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
		}
		depthAtt.imageView   = d->view();
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp      = clearDS ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		depthAtt.clearValue.depthStencil = { 1.0f, 0 };
	}

	// レンダリング開始
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	if (rtCount > 0) {
		auto* rt0 = static_cast<VkRenderTargetImpl*>(rts[0]->self);
		ri.renderArea.extent = { rt0->getWidth(), rt0->getHeight() };
	} else if (ds) {
		auto* d = static_cast<VkDepthStencilImpl*>(ds->self);
		ri.renderArea.extent = { d->getWidth(), d->getHeight() };
	}
	ri.layerCount           = 1;
	ri.colorAttachmentCount = rtCount;
	ri.pColorAttachments    = colorAtts.empty() ? nullptr : colorAtts.data();
	ri.pDepthAttachment     = ds ? &depthAtt : nullptr;
	vkCmdBeginRendering(cmd, &ri);

	// ビューポート / シザー
	if (ri.renderArea.extent.width > 0) {
		VkViewport vp = { 0,0,(float)ri.renderArea.extent.width,(float)ri.renderArea.extent.height,0,1 };
		VkRect2D   sc = { {0,0}, ri.renderArea.extent };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor (cmd, 0, 1, &sc);
	}
	vkCmdEndRendering(cmd); // NOTE: 実際は endCommands 前に EndRendering する設計が望ましい
}

// ---- GPU リソース ----
BufferHandle VkDeviceImpl::createBuffer(const GfxBufferDesc* desc) {
	VkBufRes buf; buf.sz = desc->size;
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if ((uint32_t)desc->usage & (uint32_t)BufferUsage::Vertex)  usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	if ((uint32_t)desc->usage & (uint32_t)BufferUsage::Index)   usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	if ((uint32_t)desc->usage & (uint32_t)BufferUsage::Uniform) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	if ((uint32_t)desc->usage & (uint32_t)BufferUsage::Storage) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = desc->size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(m_device, &ci, nullptr, &buf.vk) != VK_SUCCESS) return {};
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(m_device, buf.vk, &req);
	bool isHost = (desc->usage == BufferUsage::Staging) || desc->initialData;
	VkMemoryPropertyFlags mf = isHost
		? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!allocMem(req, mf, buf.mem)) { vkDestroyBuffer(m_device,buf.vk,nullptr); return {}; }
	vkBindBufferMemory(m_device, buf.vk, buf.mem, 0);
	if (desc->initialData && isHost) {
		void* p; vkMapMemory(m_device,buf.mem,0,desc->size,0,&p);
		memcpy(p,desc->initialData,desc->size); vkUnmapMemory(m_device,buf.mem);
	}
	uint64_t id = m_nextId++; m_buffers.emplace(id,buf); return {id};
}
TextureHandle VkDeviceImpl::createTexture(const GfxTextureDesc* desc) {
	VkTexRes tex;
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType=VK_IMAGE_TYPE_2D; ci.extent={desc->width,desc->height,1};
	ci.mipLevels=desc->mips; ci.arrayLayers=1; ci.format=toVkFormat(desc->format);
	ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
	ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(m_device,&ci,nullptr,&tex.vk)!=VK_SUCCESS) return {};
	VkMemoryRequirements req; vkGetImageMemoryRequirements(m_device,tex.vk,&req);
	if (!allocMem(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,tex.mem)) { vkDestroyImage(m_device,tex.vk,nullptr); return {}; }
	vkBindImageMemory(m_device,tex.vk,tex.mem,0);
	if (!makeImageView(tex.vk,toVkFormat(desc->format),VK_IMAGE_ASPECT_COLOR_BIT,tex.view)) {
		vkDestroyImage(m_device,tex.vk,nullptr); vkFreeMemory(m_device,tex.mem,nullptr); return {};
	}
	uint64_t id=m_nextId++; m_textures.emplace(id,tex); return {id};
}
ShaderHandle VkDeviceImpl::compileShader(const GfxShaderDesc* desc) {
	VkShadRes sh; sh.stage=desc->stage;
	// 実際は shaderc で GLSL → SPIR-V
	uint32_t dummy=0x07230203;
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize=4; ci.pCode=&dummy;
	vkCreateShaderModule(m_device,&ci,nullptr,&sh.vk);
	uint64_t id=m_nextId++; m_shaders.emplace(id,sh); return {id};
}
void VkDeviceImpl::destroyBuffer(BufferHandle h) {
	auto& b=m_buffers.at(h.id); vkDestroyBuffer(m_device,b.vk,nullptr); vkFreeMemory(m_device,b.mem,nullptr); m_buffers.erase(h.id);
}
void VkDeviceImpl::destroyTexture(TextureHandle h) {
	auto& t=m_textures.at(h.id); vkDestroyImageView(m_device,t.view,nullptr); vkDestroyImage(m_device,t.vk,nullptr); vkFreeMemory(m_device,t.mem,nullptr); m_textures.erase(h.id);
}
void VkDeviceImpl::destroyShader(ShaderHandle h) { vkDestroyShaderModule(m_device,m_shaders.at(h.id).vk,nullptr); m_shaders.erase(h.id); }
void VkDeviceImpl::bindShader   (ShaderHandle) {}
void VkDeviceImpl::bindTexture  (TextureHandle,uint32_t) {}

// 任意のコマンドバッファに draw を記録する汎用関数 (CommandBufferImpl から利用)
void VkDeviceImpl::drawOnCmdBuf(VkCommandBuffer cb, const GfxDrawCmd* cmd) {
	if (!cb) return;
	if (!m_buffers.count(cmd->vertexBuffer.id) || !m_buffers.count(cmd->indexBuffer.id)) return;
	auto& vb = m_buffers.at(cmd->vertexBuffer.id);
	auto& ib = m_buffers.at(cmd->indexBuffer.id);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &vb.vk, &off);
	vkCmdBindIndexBuffer(cb, ib.vk, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cb, cmd->indexCount, cmd->instanceCount,
					 cmd->firstIndex, cmd->vertexOffset, 0);
}

// 内部コマンドバッファ向けショートカット (互換維持)
void VkDeviceImpl::draw(const GfxDrawCmd* cmd) {
	drawOnCmdBuf(currentCmdBuf, cmd);
}
void VkDeviceImpl::drawFullscreen() { if (currentCmdBuf) vkCmdDraw(currentCmdBuf,3,1,0,0); }
uint64_t VkDeviceImpl::getVRAMUsageMB() const {
	VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
	VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2}; p2.pNext=&bud;
	vkGetPhysicalDeviceMemoryProperties2(m_physDevice,&p2);
	return bud.heapUsage[0]/(1024*1024);
}
uint32_t VkDeviceImpl::findMemType(uint32_t bits,VkMemoryPropertyFlags props) {
	VkPhysicalDeviceMemoryProperties mem; vkGetPhysicalDeviceMemoryProperties(m_physDevice,&mem);
	for (uint32_t i=0;i<mem.memoryTypeCount;++i)
		if ((bits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&props)==props) return i;
	return 0;
}
bool VkDeviceImpl::allocMem(VkMemoryRequirements req,VkMemoryPropertyFlags props,VkDeviceMemory& out) {
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize=req.size; ai.memoryTypeIndex=findMemType(req.memoryTypeBits,props);
	return vkAllocateMemory(m_device,&ai,nullptr,&out)==VK_SUCCESS;
}
bool VkDeviceImpl::makeImageView(VkImage img,VkFormat fmt,VkImageAspectFlags aspect,VkImageView& out) {
	VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	ci.image=img; ci.viewType=VK_IMAGE_VIEW_TYPE_2D; ci.format=fmt;
	ci.subresourceRange={aspect,0,1,0,1};
	return vkCreateImageView(m_device,&ci,nullptr,&out)==VK_SUCCESS;
}
void VkDeviceImpl::shutdown() {
	if (!m_device) return; vkDeviceWaitIdle(m_device);
	for (auto& [id,b]:m_buffers) { vkDestroyBuffer(m_device,b.vk,nullptr); vkFreeMemory(m_device,b.mem,nullptr); }
	for (auto& [id,t]:m_textures){ vkDestroyImageView(m_device,t.view,nullptr); vkDestroyImage(m_device,t.vk,nullptr); vkFreeMemory(m_device,t.mem,nullptr); }
	for (auto& [id,s]:m_shaders)   vkDestroyShaderModule(m_device,s.vk,nullptr);
	if (m_cmdPool) vkDestroyCommandPool(m_device,m_cmdPool,nullptr);
	vkDestroyDevice(m_device,nullptr); vkDestroyInstance(m_instance,nullptr);
}

// ============================================================
//  VkRenderTargetImpl 実装
// ============================================================
bool VkRenderTargetImpl::makeView(VkImage img,VkFormat fmt,VkImageView& out) {
	return m_dev->makeImageView(img,fmt,VK_IMAGE_ASPECT_COLOR_BIT,out);
}
bool VkRenderTargetImpl::allocImage() {
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType=VK_IMAGE_TYPE_2D; ci.extent={m_width,m_height,1};
	ci.mipLevels=1; ci.arrayLayers=1; ci.format=toVkFormat(m_format);
	ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
	ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (m_desc.allowUAV) ci.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
	ci.samples=static_cast<VkSampleCountFlagBits>(m_desc.sampleCount);
	ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(m_dev->device(),&ci,nullptr,&m_image)!=VK_SUCCESS) return false;
	VkMemoryRequirements req; vkGetImageMemoryRequirements(m_dev->device(),m_image,&req);
	if (!m_dev->allocMem(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,m_memory)) return false;
	vkBindImageMemory(m_dev->device(),m_image,m_memory,0);
	m_layout=VK_IMAGE_LAYOUT_UNDEFINED; m_ownImage=true;
	return makeView(m_image,toVkFormat(m_format),m_view);
}
void VkRenderTargetImpl::freeImage() {
	if (m_srvView!=VK_NULL_HANDLE) { vkDestroyImageView(m_dev->device(),m_srvView,nullptr); m_srvView=VK_NULL_HANDLE; }
	if (m_view!=VK_NULL_HANDLE)    { vkDestroyImageView(m_dev->device(),m_view,nullptr);    m_view=VK_NULL_HANDLE; }
	if (m_ownImage && m_image!=VK_NULL_HANDLE) {
		vkDestroyImage(m_dev->device(),m_image,nullptr);
		vkFreeMemory(m_dev->device(),m_memory,nullptr);
		m_image=VK_NULL_HANDLE; m_memory=VK_NULL_HANDLE;
	}
}
bool VkRenderTargetImpl::create(VkDeviceImpl* dev,const GfxRenderTargetDesc* desc) {
	m_dev=dev; m_desc=*desc;
	m_width=desc->width?desc->width:1; m_height=desc->height?desc->height:1;
	m_format=desc->format; return allocImage();
}
bool VkRenderTargetImpl::createFromSwapBuffer(VkDeviceImpl* dev,VkImage image,
											   VkFormat vkFmt,PixelFormat fmt,uint32_t w,uint32_t h) {
	m_dev=dev; m_image=image; m_format=fmt; m_width=w; m_height=h;
	m_ownImage=false; m_layout=VK_IMAGE_LAYOUT_UNDEFINED;
	return makeView(m_image,vkFmt,m_view);
}
void VkRenderTargetImpl::destroy() { freeImage(); }
bool VkRenderTargetImpl::resize(uint32_t w,uint32_t h) {
	if (!m_ownImage||(w==m_width&&h==m_height)) return true;
	m_width=w; m_height=h; m_desc.width=w; m_desc.height=h;
	freeImage(); return allocImage();
}
TextureHandle VkRenderTargetImpl::asTexture() {
	if (!m_srvView) makeView(m_image,toVkFormat(m_format),m_srvView);
	return m_srvView ? TextureHandle{reinterpret_cast<uint64_t>(m_srvView)} : TextureHandle{};
}

// ============================================================
//  VkDepthStencilImpl 実装
// ============================================================
bool VkDepthStencilImpl::allocImage() {
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType=VK_IMAGE_TYPE_2D; ci.extent={m_width,m_height,1};
	ci.mipLevels=1; ci.arrayLayers=1; ci.format=toVkFormat(m_format);
	ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
	ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
	ci.samples=static_cast<VkSampleCountFlagBits>(m_desc.sampleCount);
	ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(m_dev->device(),&ci,nullptr,&m_image)!=VK_SUCCESS) return false;
	VkMemoryRequirements req; vkGetImageMemoryRequirements(m_dev->device(),m_image,&req);
	if (!m_dev->allocMem(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,m_memory)) return false;
	vkBindImageMemory(m_dev->device(),m_image,m_memory,0);
	m_layout=VK_IMAGE_LAYOUT_UNDEFINED;
	return m_dev->makeImageView(m_image,toVkFormat(m_format),aspectOf(m_format),m_view);
}
void VkDepthStencilImpl::freeImage() {
	if (m_view)   vkDestroyImageView(m_dev->device(),m_view,nullptr);
	if (m_image)  vkDestroyImage(m_dev->device(),m_image,nullptr);
	if (m_memory) vkFreeMemory(m_dev->device(),m_memory,nullptr);
	m_view=VK_NULL_HANDLE; m_image=VK_NULL_HANDLE; m_memory=VK_NULL_HANDLE;
}
bool VkDepthStencilImpl::create(VkDeviceImpl* dev,const GfxDepthStencilDesc* desc) {
	m_dev=dev; m_desc=*desc;
	m_width=desc->width?desc->width:1; m_height=desc->height?desc->height:1;
	m_format=desc->format; return allocImage();
}
void VkDepthStencilImpl::destroy() { freeImage(); }
bool VkDepthStencilImpl::resize(uint32_t w,uint32_t h) {
	if (w==m_width&&h==m_height) return true;
	m_width=w; m_height=h; m_desc.width=w; m_desc.height=h;
	freeImage(); return allocImage();
}
TextureHandle VkDepthStencilImpl::asTexture() {
	return m_view ? TextureHandle{reinterpret_cast<uint64_t>(m_view)} : TextureHandle{};
}

// ============================================================
//  VkSwapChainImpl 実装
// ============================================================
bool VkSwapChainImpl::create(VkDeviceImpl* dev,const GfxSwapChainDesc* desc) {
	m_dev=dev; m_format=desc->format;
	// bufferCount をフレーム数として採用 (2〜MAX_FRAMES にクランプ)
	m_framesInFlight = std::max(2u, std::min(desc->bufferCount, MAX_FRAMES));
	if (desc->nativeWindowHandle) {
#if defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
		sci.hinstance=GetModuleHandle(nullptr);
		sci.hwnd=static_cast<HWND>(desc->nativeWindowHandle);
		if (vkCreateWin32SurfaceKHR(dev->instance(),&sci,nullptr,&m_surface)!=VK_SUCCESS) return false;
#endif
	}
	if (!createSyncObjects()) return false;
	return (!m_surface) || buildSwapchain(desc->width,desc->height);
}

bool VkSwapChainImpl::createSyncObjects() {
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex=m_dev->graphicsFamily();
	pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(m_dev->device(),&pci,nullptr,&m_cmdPool)!=VK_SUCCESS) return false;
	VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool=m_cmdPool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount=m_framesInFlight;
	if (vkAllocateCommandBuffers(m_dev->device(),&ai,m_cmdBufs)!=VK_SUCCESS) return false;
	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags=VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i=0;i<m_framesInFlight;++i) {
		if (vkCreateSemaphore(m_dev->device(),&sci,nullptr,&m_imageAvail[i])!=VK_SUCCESS) return false;
		if (vkCreateSemaphore(m_dev->device(),&sci,nullptr,&m_renderDone[i])!=VK_SUCCESS) return false;
		if (vkCreateFence    (m_dev->device(),&fci,nullptr,&m_inFlight[i])  !=VK_SUCCESS) return false;
	}
	return true;
}

bool VkSwapChainImpl::buildSwapchain(uint32_t w,uint32_t h) {
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_dev->physDevice(),m_surface,&caps);
	uint32_t fmtCnt=0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_dev->physDevice(),m_surface,&fmtCnt,nullptr);
	std::vector<VkSurfaceFormatKHR> fmts(fmtCnt);
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_dev->physDevice(),m_surface,&fmtCnt,fmts.data());
	VkSurfaceFormatKHR chosen=fmts[0];
	VkFormat want=toVkFormat(m_format);
	for (auto& f:fmts) if (f.format==want){chosen=f;break;}
	m_vkFormat=chosen.format;
	m_extent=(caps.currentExtent.width!=UINT32_MAX)?caps.currentExtent
			 :VkExtent2D{std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width),
						 std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height)};
	// m_framesInFlight (bufferCount) を要求枚数として使う。
	// ドライバの minImageCount を下回らないようにクランプする。
	uint32_t cnt = std::max(m_framesInFlight, caps.minImageCount);
	if (caps.maxImageCount > 0) cnt = std::min(cnt, caps.maxImageCount);
	VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface=m_surface; ci.minImageCount=cnt;
	ci.imageFormat=chosen.format; ci.imageColorSpace=chosen.colorSpace;
	ci.imageExtent=m_extent; ci.imageArrayLayers=1;
	ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform=caps.currentTransform; ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE; ci.oldSwapchain=m_swapchain;
	VkSwapchainKHR newSC=VK_NULL_HANDLE;
	if (vkCreateSwapchainKHR(m_dev->device(),&ci,nullptr,&newSC)!=VK_SUCCESS) return false;
	if (m_swapchain) vkDestroySwapchainKHR(m_dev->device(),m_swapchain,nullptr);
	m_swapchain=newSC;
	vkGetSwapchainImagesKHR(m_dev->device(),m_swapchain,&m_imageCount,nullptr);
	std::vector<VkImage> images(m_imageCount);
	vkGetSwapchainImagesKHR(m_dev->device(),m_swapchain,&m_imageCount,images.data());
	for (uint32_t i=0;i<m_imageCount;++i) {
		if (m_backRTs[i]) { m_backRTs[i]->destroy(); delete m_backRTs[i]; }
		m_backRTs[i]=new VkRenderTargetImpl();
		if (!m_backRTs[i]->createFromSwapBuffer(m_dev,images[i],m_vkFormat,m_format,m_extent.width,m_extent.height)) return false;
	}
	return true;
}

void VkSwapChainImpl::destroySwapchain() {
	for (uint32_t i=0;i<m_imageCount;++i) if (m_backRTs[i]){m_backRTs[i]->destroy();delete m_backRTs[i];m_backRTs[i]=nullptr;}
	if (m_swapchain) vkDestroySwapchainKHR(m_dev->device(),m_swapchain,nullptr); m_swapchain=VK_NULL_HANDLE;
}

bool VkSwapChainImpl::resize(uint32_t w,uint32_t h) {
	if (!m_surface) return true;
	vkDeviceWaitIdle(m_dev->device()); destroySwapchain();
	return buildSwapchain(w,h);
}

bool VkSwapChainImpl::acquireNextBuffer() {
	vkWaitForFences(m_dev->device(),1,&m_inFlight[m_frameIdx],VK_TRUE,UINT64_MAX);
	if (m_swapchain)
		vkAcquireNextImageKHR(m_dev->device(),m_swapchain,UINT64_MAX,m_imageAvail[m_frameIdx],VK_NULL_HANDLE,&m_imageIdx);
	vkResetFences(m_dev->device(),1,&m_inFlight[m_frameIdx]);
	VkCommandBuffer cmd=m_cmdBufs[m_frameIdx];
	vkResetCommandBuffer(cmd,0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd,&bi);
	m_dev->currentCmdBuf=cmd;
	return true;
}

IRenderTarget* VkSwapChainImpl::getRenderTarget() {
	if (!m_backRTs[m_imageIdx]) return nullptr;
	m_currentRT.vtable=VkRenderTargetImpl::getVTable();
	m_currentRT.self  =m_backRTs[m_imageIdx];
	return &m_currentRT;
}

void VkSwapChainImpl::present() {
	VkCommandBuffer cmd=m_cmdBufs[m_frameIdx];
	// バックバッファを Present レイアウトに遷移
	auto* rt=m_backRTs[m_imageIdx];
	if (rt->layout()!=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
		VkImageMemoryBarrier2 bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		bar.srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		bar.srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		bar.oldLayout=rt->layout(); bar.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		bar.image=rt->image(); bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
		VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.imageMemoryBarrierCount=1; dep.pImageMemoryBarriers=&bar;
		vkCmdPipelineBarrier2(cmd,&dep);
		rt->setLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	vkEndCommandBuffer(cmd);
	m_dev->currentCmdBuf=VK_NULL_HANDLE;

	VkPipelineStageFlags wait=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount=m_swapchain?1u:0u; si.pWaitSemaphores=&m_imageAvail[m_frameIdx];
	si.pWaitDstStageMask=&wait; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
	si.signalSemaphoreCount=m_swapchain?1u:0u; si.pSignalSemaphores=&m_renderDone[m_frameIdx];
	vkQueueSubmit(m_dev->graphicsQueue(),1,&si,m_inFlight[m_frameIdx]);

	if (m_swapchain) {
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&m_renderDone[m_frameIdx];
		pi.swapchainCount=1; pi.pSwapchains=&m_swapchain; pi.pImageIndices=&m_imageIdx;
		vkQueuePresentKHR(m_dev->presentQueue(),&pi);
	}
	m_frameIdx=(m_frameIdx+1)%m_framesInFlight;
}

void VkSwapChainImpl::destroy() {
	vkDeviceWaitIdle(m_dev->device()); destroySwapchain();
	for (uint32_t i=0;i<m_framesInFlight;++i) {
		vkDestroySemaphore(m_dev->device(),m_imageAvail[i],nullptr);
		vkDestroySemaphore(m_dev->device(),m_renderDone[i],nullptr);
		vkDestroyFence    (m_dev->device(),m_inFlight[i],  nullptr);
	}
	if (m_cmdPool) vkDestroyCommandPool(m_dev->device(),m_cmdPool,nullptr);
	if (m_surface) vkDestroySurfaceKHR(m_dev->instance(),m_surface,nullptr);
}
