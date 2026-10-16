// ============================================================
//  backends/dx12/DX12Export.cpp
//  vtable の定義と DLL のエクスポート関数
// ============================================================

#include "DX12Device.h"

// ============================================================
//  vtable 定義 — IRenderTarget
// ============================================================
static const IRenderTargetVTable k_DX12RTVTable =
{
	[](void* s)
	{
		auto* impl = static_cast<DX12RenderTargetImpl*>(s);
		// バックバッファ RT は SwapChain が所有する。アプリからの destroy は無視する
		if (impl->isSwapChainBuffer())
		{
			fprintf(stderr, "[GfxEngine] IRenderTarget::destroy: SwapChain のバックバッファは破棄できません (無視しました)\n");
			return;
		}
		impl->destroy();
		delete impl;   // m_iface (アプリに返したラッパー) も一緒に解放される
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12RenderTargetImpl*>(s)->resize(w, h);
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getFormat();
	},
	[](void* s) -> TextureHandle
	{
		return static_cast<DX12RenderTargetImpl*>(s)->asTexture();
	},
};
const IRenderTargetVTable* DX12RenderTargetImpl::getVTable() { return &k_DX12RTVTable; }

// ============================================================
//  vtable 定義 — IDepthStencil
// ============================================================
static const IDepthStencilVTable k_DX12DSVTable =
{
	[](void* s)
	{
		static_cast<DX12DepthStencilImpl*>(s)->destroy();
		delete static_cast<DX12DepthStencilImpl*>(s);
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12DepthStencilImpl*>(s)->resize(w, h);
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getFormat();
	},
	[](void* s) -> TextureHandle
	{
		return static_cast<DX12DepthStencilImpl*>(s)->asTexture();
	},
};
const IDepthStencilVTable* DX12DepthStencilImpl::getVTable() { return &k_DX12DSVTable; }

// ============================================================
//  vtable 定義 — ISwapChain
// ============================================================
static const ISwapChainVTable k_DX12SCVTable =
{
	[](void* s)
	{
		static_cast<DX12SwapChainImpl*>(s)->destroy();
		delete static_cast<DX12SwapChainImpl*>(s);
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12SwapChainImpl*>(s)->resize(w, h);
	},
	[](void* s) -> bool
	{
		return static_cast<DX12SwapChainImpl*>(s)->acquireNextBuffer();
	},
	[](void* s) -> IRenderTarget*
	{
		return static_cast<DX12SwapChainImpl*>(s)->getRenderTarget();
	},
	[](void* s)
	{
		static_cast<DX12SwapChainImpl*>(s)->present();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getFormat();
	},
};
const ISwapChainVTable* DX12SwapChainImpl::getVTable() { return &k_DX12SCVTable; }

// ============================================================
//  vtable 定義 — ICommandBuffer
// ============================================================
static const ICommandBufferVTable k_DX12CmdVTable =
{
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->begin();
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->end();
	},
	[](void* s, void* p)
	{
		static_cast<DX12CommandBufferImpl*>(s)->executeBundle(
			static_cast<DX12CommandBufferImpl*>(p));
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->release();
	},
	[](void* s, uint32_t n, IRenderTarget* const* rts, IDepthStencil* ds, bool cr, bool cd)
	{
		static_cast<DX12CommandBufferImpl*>(s)->setRenderTargets(n, rts, ds, cr, cd);
	},
	[](void* s, IPipeline* p)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindPipeline(p);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindTexture(h, slot);
	},
	[](void* s, SamplerHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindSampler(h, slot);
	},
	[](void* s, BufferHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindUniformBuffer(h, slot);
	},
	[](void* s, Topology t)
	{
		static_cast<DX12CommandBufferImpl*>(s)->setTopology(t);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindUAV(h, slot);
	},
	[](void* s, RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output,
	   TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h)
	{
		static_cast<DX12CommandBufferImpl*>(s)->dispatchRays(pipe, tlas, output, depthTex, rtCB, w, h);
	},
	[](void* s, TextureHandle h)
	{
		static_cast<DX12CommandBufferImpl*>(s)->transitionToUAV(h);
	},
	[](void* s, const GfxDrawCmd* cmd)
	{
		static_cast<DX12CommandBufferImpl*>(s)->draw(cmd);
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->drawFullscreen();
	},
	[](const void* s) -> CommandBufferType
	{
		return static_cast<const DX12CommandBufferImpl*>(s)->isDirect()
			? CommandBufferType::Direct : CommandBufferType::Bundle;
	},
	// ---- ABI v2 ----
	[](void* s, uint32_t i, uint32_t n, const void* d, uint32_t o) { static_cast<DX12CommandBufferImpl*>(s)->setRootConstants(i, n, d, o); },
	[](void* s, uint32_t i, BufferHandle b, uint64_t o)            { static_cast<DX12CommandBufferImpl*>(s)->setRootDescriptor(i, b, o); },
	[](void* s, uint32_t i, const GfxDescriptor* d, uint32_t n)    { static_cast<DX12CommandBufferImpl*>(s)->setDescriptorTable(i, d, n); },
	[](void* s, uint32_t first, uint32_t n, const GfxVertexBufferBinding* b) { static_cast<DX12CommandBufferImpl*>(s)->setVertexBuffers(first, n, b); },
	[](void* s, const GfxIndexBufferBinding* b)                    { static_cast<DX12CommandBufferImpl*>(s)->setIndexBuffer(b); },
	[](void* s, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi)  { static_cast<DX12CommandBufferImpl*>(s)->drawInstanced(v, i, fv, fi); },
	[](void* s, uint32_t n, uint32_t i, uint32_t fi, int32_t bv, uint32_t fin) { static_cast<DX12CommandBufferImpl*>(s)->drawIndexedInstanced(n, i, fi, bv, fin); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->drawSignature(), d, false); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->drawIndexedSignature(), d, false); },
	[](void* s, uint32_t x, uint32_t y, uint32_t z)                { static_cast<DX12CommandBufferImpl*>(s)->dispatch(x, y, z); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->dispatchSignature(), d, true); },
	[](void* s, const GfxBarrierDesc* b, uint32_t n)               { static_cast<DX12CommandBufferImpl*>(s)->barrier(b, n); },
	[](void* s, uint32_t n, const GfxViewport* v)                  { static_cast<DX12CommandBufferImpl*>(s)->setViewports(n, v); },
	[](void* s, uint32_t n, const GfxRect* r)                      { static_cast<DX12CommandBufferImpl*>(s)->setScissors(n, r); },
	[](void* s, uint32_t ref)                                      { static_cast<DX12CommandBufferImpl*>(s)->setStencilRef(ref); },
	[](void* s, const float* rgba)                                 { static_cast<DX12CommandBufferImpl*>(s)->setBlendFactor(rgba); },
	[](void* s, BufferHandle d, uint64_t dof, BufferHandle src, uint64_t sof, uint64_t n) { static_cast<DX12CommandBufferImpl*>(s)->copyBuffer(d, dof, src, sof, n); },
	[](void* s, GfxResourceRef d, uint32_t ds, uint32_t x, uint32_t y, uint32_t z, GfxResourceRef src, uint32_t ss, const GfxBox* box)
	   { static_cast<DX12CommandBufferImpl*>(s)->copyTexture(d, ds, x, y, z, src, ss, box); },
	[](void* s, IRenderTarget* rt, const float* c, uint32_t n, const GfxRect* r) { static_cast<DX12CommandBufferImpl*>(s)->clearRenderTarget(rt, c, n, r); },
	[](void* s, IDepthStencil* ds, GfxClearFlags f, float d, uint8_t st, uint32_t n, const GfxRect* r)
	   { static_cast<DX12CommandBufferImpl*>(s)->clearDepthStencil(ds, f, d, st, n, r); },
	[](void* s, const char* name)                                  { static_cast<DX12CommandBufferImpl*>(s)->beginEvent(name); },
	[](void* s)                                                    { static_cast<DX12CommandBufferImpl*>(s)->endEvent(); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->writeTimestamp(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->beginQuery(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->endQuery(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t f, uint32_t n)         { static_cast<DX12CommandBufferImpl*>(s)->resolveQueries(p, f, n); },
	[](void* s, uint32_t n, const ViewHandle* rtvs, ViewHandle dsv) { static_cast<DX12CommandBufferImpl*>(s)->setRenderTargetViews(n, rtvs, dsv); },
	[](void* s, ViewHandle v, const float* c, uint32_t n, const GfxRect* r) { static_cast<DX12CommandBufferImpl*>(s)->clearRenderTargetView(v, c, n, r); },
	[](void* s, ViewHandle v, GfxClearFlags f, float d, uint8_t st, uint32_t n, const GfxRect* r)
	   { static_cast<DX12CommandBufferImpl*>(s)->clearDepthStencilView(v, f, d, st, n, r); },
	[](void* s, GfxResourceRef d, uint32_t ds, GfxResourceRef src, uint32_t ss, GfxFormat f) { static_cast<DX12CommandBufferImpl*>(s)->resolve(d, ds, src, ss, f); },
};
const ICommandBufferVTable* DX12CommandBufferImpl::getVTable() { return &k_DX12CmdVTable; }

// ============================================================
//  vtable 定義 — GfxDevice
// ============================================================
static const GfxDeviceVTable k_DX12DevVTable =
{
	[](void* s, const GfxDeviceConfig* c) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->init(c);
	},
	[](void* s)
	{
		static_cast<DX12DeviceImpl*>(s)->shutdown();
	},
	[](void* s, const GfxSwapChainDesc* d) -> ISwapChain*
	{
		return static_cast<DX12DeviceImpl*>(s)->createSwapChain(d);
	},
	[](void* s, const GfxRenderTargetDesc* d) -> IRenderTarget*
	{
		return static_cast<DX12DeviceImpl*>(s)->createRenderTarget(d);
	},
	[](void* s, const GfxDepthStencilDesc* d) -> IDepthStencil*
	{
		return static_cast<DX12DeviceImpl*>(s)->createDepthStencil(d);
	},
	[](void* s, CommandBufferType t) -> ICommandBuffer*
	{
		return static_cast<DX12DeviceImpl*>(s)->allocCommandBuffer(t);
	},
	[](void* s, ICommandBuffer* const* cbs, uint32_t n)
	{
		static_cast<DX12DeviceImpl*>(s)->submitCommandBuffers(cbs, n);
	},
	[](void* s, const GfxBufferDesc* d) -> BufferHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBuffer(d);
	},
	[](void* s, const GfxTextureDesc* d) -> TextureHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTexture(d);
	},
	[](void* s, const GfxShaderDesc* d) -> ShaderHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->compileShader(d);
	},
	[](void* s, const GfxSamplerDesc* d) -> SamplerHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createSampler(d);
	},
	[](void* s, BufferHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBuffer(h);
	},
	[](void* s, TextureHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyTexture(h);
	},
	[](void* s, ShaderHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyShader(h);
	},
	[](void* s, SamplerHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroySampler(h);
	},
	[](void* s, BufferHandle h, const void* data, uint64_t size) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->updateBuffer(h, data, size);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12DeviceImpl*>(s)->bindTexture(h, slot);
	},
	[](void* s, const GfxDrawCmd* cmd)
	{
		static_cast<DX12DeviceImpl*>(s)->draw(cmd);
	},
	[](void* s)
	{
		static_cast<DX12DeviceImpl*>(s)->drawFullscreen();
	},
	[](const void* s) -> GraphicsAPI
	{
		return static_cast<const DX12DeviceImpl*>(s)->getAPI();
	},
	[](const void* s) -> const char*
	{
		return static_cast<const DX12DeviceImpl*>(s)->getAdapterName();
	},
	[](const void* s) -> uint64_t
	{
		return static_cast<const DX12DeviceImpl*>(s)->getVRAMUsageMB();
	},
	[](void* s, const GfxPipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createPipeline(d);
	},
	[](void* s, PipelineHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyPipeline(h);
	},
	[](void* s, const GfxBLASDesc* d) -> BLASHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBLAS(d);
	},
	[](void* s, BLASHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBLAS(h);
	},
	[](void* s, const GfxTLASInstance* inst, uint32_t n) -> TLASHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTLAS(inst, n);
	},
	[](void* s, TLASHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyTLAS(h);
	},
	[](void* s, TLASHandle h, const GfxTLASInstance* inst, uint32_t n)
	{
		static_cast<DX12DeviceImpl*>(s)->updateTLAS(h, inst, n);
	},
	[](void* s, const GfxRTPipelineDesc* d) -> RTPipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createRTPipeline(d);
	},
	[](void* s, RTPipelineHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyRTPipeline(h);
	},
	[](void* s, const GfxBindingLayoutDesc* d) -> BindingLayoutHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBindingLayout(d);
	},
	[](void* s, BindingLayoutHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBindingLayout(h);
	},
	[](void* s, const GfxGraphicsPipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createGraphicsPipeline(d);
	},
	[](void* s, const GfxComputePipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createComputePipeline(d);
	},
	[](void* s, const GfxQueryPoolDesc* d) -> QueryPoolHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createQueryPool(d);
	},
	[](void* s, QueryPoolHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyQueryPool(h);
	},
	[](void* s, QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->readQueryResults(h, first, count, out);
	},
	[](const void* s) -> uint64_t
	{
		return static_cast<const DX12DeviceImpl*>(s)->getTimestampFrequency();
	},
	[](void* s, const GfxTextureResourceDesc* d) -> TextureHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTextureResource(d);
	},
	[](void* s, const GfxViewDesc* d) -> ViewHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createView(d);
	},
	[](void* s, ViewHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyView(h);
	},
	[](void* s, const GfxSamplerStateDesc* d) -> SamplerHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createSamplerState(d);
	},
	[](void* s, ShaderHandle h, GfxShaderReflection* out) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->getShaderReflection(h, out);
	},
	[](void* s, ShaderHandle h, char* buffer, uint32_t bufferSize) -> uint32_t
	{
		return static_cast<DX12DeviceImpl*>(s)->getShaderDependencies(h, buffer, bufferSize);
	},
	[](void* s, ShaderHandle h, const GfxShaderDesc* d) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->updateShader(h, d);
	},
	[](void* s, PipelineHandle h, const GfxGraphicsPipelineDesc* d) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->updateGraphicsPipeline(h, d);
	},
	[](void* s, PipelineHandle h, const GfxComputePipelineDesc* d) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->updateComputePipeline(h, d);
	},
	[](const void* s, PipelineHandle h, GfxGraphicsPipelineDesc* out) -> bool
	{
		return static_cast<const DX12DeviceImpl*>(s)->getGraphicsPipelineDesc(h, out);
	},
	[](const void* s, PipelineHandle h, GfxComputePipelineDesc* out) -> bool
	{
		return static_cast<const DX12DeviceImpl*>(s)->getComputePipelineDesc(h, out);
	},
};
const GfxDeviceVTable* DX12DeviceImpl::getVTable() { return &k_DX12DevVTable; }

GFX_PLUGIN_EXPORT_FUNCTIONS(DX12DeviceImpl)
