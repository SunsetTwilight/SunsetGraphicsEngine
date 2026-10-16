// ============================================================
//  backends/dx12/DX12Resources.cpp
//  テクスチャ / ビュー / サンプラー (v2)、RenderTarget / DepthStencil
// ============================================================

#include "DX12Device.h"

// ============================================================
//  DX12DeviceImpl — テクスチャ / ビュー / サンプラー の v2 (Phase 2b-2)
// ============================================================

bool DX12DeviceImpl::checkSampleCount(DXGI_FORMAT fmt, uint32_t count, uint32_t quality, const char* fn)
{
	if (count <= 1)
		return true;
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {};
	q.Format      = fmt;
	q.SampleCount = count;
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q)))
	 || q.NumQualityLevels == 0 || quality >= q.NumQualityLevels)
	{
		fprintf(stderr, "[GfxEngine] %s: フォーマット %d は sampleCount=%u / quality=%u の MSAA に対応していません\n",
			fn, static_cast<int>(fmt), count, quality);
		return false;
	}
	return true;
}

// mip 0 / サブリソース 0 に初期データを転送し、finalState に遷移させる (同期)
bool DX12DeviceImpl::uploadSubresource0(
	ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t width, uint32_t height,
	const void* data, uint32_t rowPitch, D3D12_RESOURCE_STATES finalState)
{
	const uint32_t bpp = dxgiBytesPerPixel(fmt);
	if (bpp == 0)
	{
		fprintf(stderr, "[GfxEngine] createTexture: フォーマット %d の初期データの転送には未対応です\n", static_cast<int>(fmt));
		return false;
	}
	const uint32_t rowBytes    = width * bpp;
	const uint32_t srcRowPitch = rowPitch > 0 ? rowPitch : rowBytes;
	const UINT     dstRowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

	ComPtr<ID3D12Resource> upload = createBufferResource(m_device.Get(), static_cast<UINT64>(dstRowPitch) * height, D3D12_HEAP_TYPE_UPLOAD);
	if (!upload)
		return false;
	uint8_t* dst = nullptr;
	if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&dst))))
		return false;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + static_cast<size_t>(row) * dstRowPitch,
		       static_cast<const uint8_t*>(data) + static_cast<size_t>(row) * srcRowPitch, rowBytes);
	upload->Unmap(0, nullptr);

	ComPtr<ID3D12CommandAllocator>     alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	if (!createTransientCmdList(m_device.Get(), alloc, cl))
		return false;

	D3D12_TEXTURE_COPY_LOCATION dl = {};
	dl.pResource        = res;
	dl.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dl.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION sl = {};
	sl.pResource                          = upload.Get();
	sl.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sl.PlacedFootprint.Footprint.Format   = fmt;
	sl.PlacedFootprint.Footprint.Width    = width;
	sl.PlacedFootprint.Footprint.Height   = height;
	sl.PlacedFootprint.Footprint.Depth    = 1;
	sl.PlacedFootprint.Footprint.RowPitch = dstRowPitch;
	cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
	if (finalState != D3D12_RESOURCE_STATE_COPY_DEST)
		transition(cl.Get(), res, D3D12_RESOURCE_STATE_COPY_DEST, finalState);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());
	return true;
}

TextureHandle DX12DeviceImpl::createTextureResource(const GfxTextureResourceDesc* src)
{
	GfxTextureResourceDesc d;
	if (!readVersionedDesc(src, d, "createTextureResource"))
		return {};

	const bool usageSRV = (d.usage & GfxTextureUsage::ShaderResource)  != GfxTextureUsage::None;
	const bool usageRT  = (d.usage & GfxTextureUsage::RenderTarget)    != GfxTextureUsage::None;
	const bool usageDS  = (d.usage & GfxTextureUsage::DepthStencil)    != GfxTextureUsage::None;
	const bool usageUAV = (d.usage & GfxTextureUsage::UnorderedAccess) != GfxTextureUsage::None;
	const bool msaa     = d.sampleCount > 1;
	const bool cube     = d.dimension == GfxTextureDimension::TextureCube;
	const char* name    = d.debugName ? d.debugName : "unnamed";

	if (d.width == 0 || d.height == 0 || d.depthOrArraySize == 0 || d.sampleCount == 0)
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: サイズ / サンプル数が 0 です (%s)\n", name);
		return {};
	}
	if (cube && (d.depthOrArraySize % 6) != 0)
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: Cube の配列数は 6 の倍数にしてください (%s)\n", name);
		return {};
	}
	if (msaa && (d.dimension != GfxTextureDimension::Texture2D || d.mipLevels > 1 || usageUAV || d.initialData))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: MSAA は 2D・mip 1・UAV なし・初期データなしのみです (%s)\n", name);
		return {};
	}
	if (usageDS && (usageRT || usageUAV))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: DepthStencil は RenderTarget / UnorderedAccess と併用できません (%s)\n", name);
		return {};
	}

	const DXGI_FORMAT      requested = static_cast<DXGI_FORMAT>(d.format);
	const DX12DepthFormats depthFmts = splitDepthFormat(requested);
	// 深度フォーマットを SRV でも読むときは Typeless でリソースを作る
	const DXGI_FORMAT resourceFmt = (isDepthDXGI(requested) && usageSRV) ? depthFmts.resource : requested;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension          = d.dimension == GfxTextureDimension::Texture1D ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
	                      : d.dimension == GfxTextureDimension::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
	                      :                                                 D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width              = d.width;
	rd.Height             = d.dimension == GfxTextureDimension::Texture1D ? 1 : d.height;
	rd.DepthOrArraySize   = static_cast<UINT16>(d.depthOrArraySize);
	rd.MipLevels          = static_cast<UINT16>(d.mipLevels);
	rd.Format             = resourceFmt;
	rd.SampleDesc.Count   = d.sampleCount;
	rd.SampleDesc.Quality = d.sampleQuality;
	if (usageRT)  rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (usageDS)  rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (usageUAV) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (usageDS && !usageSRV) rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

	if (msaa && !checkSampleCount(usageDS ? depthFmts.dsv : resourceFmt, d.sampleCount, d.sampleQuality, "createTextureResource"))
		return {};

	// 最適化クリア値
	D3D12_CLEAR_VALUE cv = {};
	const bool useClear = d.useClearValue && (usageRT || usageDS);
	if (useClear)
	{
		cv.Format = d.clearValue.format != GfxFormat::Unknown ? static_cast<DXGI_FORMAT>(d.clearValue.format)
		          : usageDS ? depthFmts.dsv : resourceFmt;
		if (usageDS)
		{
			cv.DepthStencil.Depth   = d.clearValue.depth;
			cv.DepthStencil.Stencil = d.clearValue.stencil;
		}
		else
		{
			memcpy(cv.Color, d.clearValue.color, sizeof(cv.Color));
		}
	}

	// 状態: 指定がなければ用途から決める
	D3D12_RESOURCE_STATES finalState = static_cast<D3D12_RESOURCE_STATES>(d.initialState);
	if (d.initialState == GfxResourceState::Tracked)
		finalState = usageRT  ? D3D12_RESOURCE_STATE_RENDER_TARGET
		           : usageDS  ? D3D12_RESOURCE_STATE_DEPTH_WRITE
		           : usageSRV ? k_ShaderReadState
		           : usageUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		           :            D3D12_RESOURCE_STATE_COMMON;
	const D3D12_RESOURCE_STATES createState = d.initialData ? D3D12_RESOURCE_STATE_COPY_DEST : finalState;

	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	DX12TexRes tex;
	tex.slot = UINT32_MAX;
	if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, createState,
			useClear ? &cv : nullptr, IID_PPV_ARGS(&tex.res))))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: CreateCommittedResource に失敗しました (%s)\n", name);
		return {};
	}
	setDebugName(tex.res.Get(), d.debugName);

	if (d.initialData && !uploadSubresource0(tex.res.Get(), resourceFmt, d.width, rd.Height, d.initialData, d.rowPitch, finalState))
		return {};

	tex.dsvFormat = depthFmts.dsv;
	tex.srvFormat = isDepthDXGI(requested) ? depthFmts.srv : requested;
	const UINT arraySize = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.depthOrArraySize;

	// 既定の SRV (全体)
	if (usageSRV)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format                  = tex.srvFormat;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		const UINT mips = d.initialData ? 1u : static_cast<UINT>(-1);   // 初期データは mip 0 のみ
		if (d.dimension == GfxTextureDimension::Texture1D)
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY; sv.Texture1DArray.MipLevels = mips; sv.Texture1DArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;      sv.Texture1D.MipLevels      = mips; }
		}
		else if (d.dimension == GfxTextureDimension::Texture3D)
		{
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sv.Texture3D.MipLevels = mips;
		}
		else if (cube)
		{
			if (arraySize > 6) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; sv.TextureCubeArray.MipLevels = mips; sv.TextureCubeArray.NumCubes = arraySize / 6; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;      sv.TextureCube.MipLevels      = mips; }
		}
		else if (msaa)
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; sv.Texture2DMSArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; }
		}
		else
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Texture2DArray.MipLevels = mips; sv.Texture2DArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;      sv.Texture2D.MipLevels      = mips; }
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(tex.slot);
		if (tex.slot == UINT32_MAX)
			return {};
		m_device->CreateShaderResourceView(tex.res.Get(), &sv, cpu);
		publishStaticSRV(tex.slot);
	}

	// 既定の UAV (mip 0、全スライス)
	if (usageUAV)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format = requested;
		if (d.dimension == GfxTextureDimension::Texture1D)
		{
			if (arraySize > 1) { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; uv.Texture1DArray.ArraySize = arraySize; }
			else               { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; }
		}
		else if (d.dimension == GfxTextureDimension::Texture3D)
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; uv.Texture3D.WSize = d.depthOrArraySize;
		}
		else if (arraySize > 1)
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uv.Texture2DArray.ArraySize = arraySize;
		}
		else
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(tex.uavSlot);
		if (tex.uavSlot != UINT32_MAX)
		{
			m_device->CreateUnorderedAccessView(tex.res.Get(), nullptr, &uv, cpu);
			publishStaticSRV(tex.uavSlot);
		}
	}

	registerResourceState(tex.res.Get(), finalState);

	const uint64_t id = m_nextId++;
	m_textures.emplace(id, std::move(tex));
	return TextureHandle{ id };
}

// ビューの対象リソースと、既定のフォーマット (Unknown のときに使う) を求める
bool DX12DeviceImpl::resolveViewResource(const GfxResourceRef& ref, GfxViewType type,
	ComPtr<ID3D12Resource>& outRes, DXGI_FORMAT& outDefaultFormat, bool& outIsBuffer)
{
	outIsBuffer = false;
	switch (ref.kind)
	{
	case GfxResourceKind::Buffer:
	{
		const DX12BufRes* buf = getBuffer(ref.handle);
		if (!buf || !buf->res) return false;
		outRes           = buf->res;
		outDefaultFormat = DXGI_FORMAT_UNKNOWN;
		outIsBuffer      = true;
		return true;
	}
	case GfxResourceKind::Texture:
	{
		auto it = m_textures.find(ref.handle);
		if (it != m_textures.end())
		{
			outRes           = it->second.res;
			outDefaultFormat = (type == GfxViewType::DSV) ? it->second.dsvFormat : it->second.srvFormat;
			if (outDefaultFormat == DXGI_FORMAT_UNKNOWN)
				outDefaultFormat = outRes->GetDesc().Format;
			return true;
		}
		ID3D12Resource* raw = getTexResource(ref.handle);   // asTexture() のハンドル
		if (!raw) return false;
		outRes = raw;
		break;
	}
	case GfxResourceKind::RenderTarget:
	{
		auto* rt = reinterpret_cast<IRenderTarget*>(ref.handle);
		if (!rt || !rt->self) return false;
		outRes = static_cast<DX12RenderTargetImpl*>(rt->self)->resource();
		break;
	}
	case GfxResourceKind::DepthStencil:
	{
		auto* ds = reinterpret_cast<IDepthStencil*>(ref.handle);
		if (!ds || !ds->self) return false;
		outRes = static_cast<DX12DepthStencilImpl*>(ds->self)->resource();
		break;
	}
	default:
		return false;
	}
	if (!outRes)
		return false;
	// v1 の RT / DS: Typeless の深度は用途に合わせて読み替える
	const DXGI_FORMAT f = outRes->GetDesc().Format;
	switch (f)
	{
	case DXGI_FORMAT_R32_TYPELESS:   outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D32_FLOAT         : DXGI_FORMAT_R32_FLOAT;             break;
	case DXGI_FORMAT_R24G8_TYPELESS: outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
	case DXGI_FORMAT_R16_TYPELESS:   outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D16_UNORM         : DXGI_FORMAT_R16_UNORM;             break;
	default:                         outDefaultFormat = f; break;
	}
	return true;
}

ViewHandle DX12DeviceImpl::createView(const GfxViewDesc* src)
{
	GfxViewDesc d;
	if (!readVersionedDesc(src, d, "createView"))
		return {};
	const char* name = d.debugName ? d.debugName : "unnamed";

	DX12ViewRes view;
	DXGI_FORMAT defaultFmt = DXGI_FORMAT_UNKNOWN;
	bool        isBuffer   = false;
	if (!resolveViewResource(d.resource, d.type, view.res, defaultFmt, isBuffer))
	{
		fprintf(stderr, "[GfxEngine] createView: resource が無効です (%s)\n", name);
		return {};
	}
	if (isBuffer != (d.dimension == GfxViewDimension::Buffer))
	{
		fprintf(stderr, "[GfxEngine] createView: バッファには Buffer 次元、テクスチャにはテクスチャの次元を指定してください (%s)\n", name);
		return {};
	}

	const D3D12_RESOURCE_DESC rd = view.res->GetDesc();
	const DXGI_FORMAT fmt  = d.format != GfxFormat::Unknown ? static_cast<DXGI_FORMAT>(d.format)
	                       : d.rawBuffer ? DXGI_FORMAT_R32_TYPELESS : defaultFmt;
	const UINT  mip        = d.mostDetailedMip;
	const UINT  mips       = d.mipLevels;                                    // UINT_MAX = 残り全部
	const UINT  first      = d.firstArraySlice;
	const UINT  totalSlices = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<UINT>(1, rd.DepthOrArraySize >> mip)
	                                                                             : rd.DepthOrArraySize;
	const UINT  slices     = d.arraySize == GFX_ALL_MIPS ? (totalSlices > first ? totalSlices - first : 0) : d.arraySize;

	view.type   = d.type;
	view.format = fmt;
	view.width  = std::max<UINT>(1, static_cast<UINT>(rd.Width >> mip));
	view.height = std::max<UINT>(1, rd.Height >> mip);

	switch (d.type)
	{
	case GfxViewType::SRV:
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format                  = fmt;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		switch (d.dimension)
		{
		case GfxViewDimension::Buffer:
			sv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
			sv.Buffer.FirstElement        = d.firstElement;
			sv.Buffer.NumElements         = d.numElements;
			sv.Buffer.StructureByteStride = d.rawBuffer ? 0 : d.structureByteStride;
			sv.Buffer.Flags               = d.rawBuffer ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
			if (d.structureByteStride > 0 && !d.rawBuffer) sv.Format = DXGI_FORMAT_UNKNOWN;
			break;
		case GfxViewDimension::Texture1D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
			sv.Texture1D     = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::Texture1DArray:
			sv.ViewDimension  = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
			sv.Texture1DArray = { mip, mips, first, slices, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D     = { mip, mips, d.planeSlice, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2DArray:
			sv.ViewDimension  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			sv.Texture2DArray = { mip, mips, first, slices, d.planeSlice, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2DMS:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			break;
		case GfxViewDimension::Texture2DMSArray:
			sv.ViewDimension    = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
			sv.Texture2DMSArray = { first, slices };
			break;
		case GfxViewDimension::Texture3D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			sv.Texture3D     = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::TextureCube:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			sv.TextureCube   = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::TextureCubeArray:
			sv.ViewDimension    = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
			sv.TextureCubeArray = { mip, mips, first, d.arraySize == GFX_ALL_MIPS ? slices / 6 : d.arraySize, d.minLODClamp };
			break;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateShaderResourceView(view.res.Get(), &sv, cpu);
		publishStaticSRV(view.slot);
		view.state = isBuffer ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
		                      : k_ShaderReadState;
		break;
	}
	case GfxViewType::UAV:
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format = fmt;
		switch (d.dimension)
		{
		case GfxViewDimension::Buffer:
			uv.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
			uv.Buffer.FirstElement        = d.firstElement;
			uv.Buffer.NumElements         = d.numElements;
			uv.Buffer.StructureByteStride = d.rawBuffer ? 0 : d.structureByteStride;
			uv.Buffer.Flags               = d.rawBuffer ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
			if (d.structureByteStride > 0 && !d.rawBuffer) uv.Format = DXGI_FORMAT_UNKNOWN;
			break;
		case GfxViewDimension::Texture1D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;  uv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; uv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;  uv.Texture2D = { mip, d.planeSlice }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uv.Texture2DArray = { mip, first, slices, d.planeSlice }; break;
		case GfxViewDimension::Texture3D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;  uv.Texture3D = { mip, first, slices }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: MSAA の UAV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateUnorderedAccessView(view.res.Get(), nullptr, &uv, cpu);
		publishStaticSRV(view.slot);
		view.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		break;
	}
	case GfxViewType::RTV:
	{
		D3D12_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = fmt;
		switch (d.dimension)
		{
		case GfxViewDimension::Texture1D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;        rv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:   rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;   rv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;        rv.Texture2D = { mip, d.planeSlice }; break;
		case GfxViewDimension::Texture2DMS:      rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;      break;
		case GfxViewDimension::Texture2DMSArray: rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; rv.Texture2DMSArray = { first, slices }; break;
		case GfxViewDimension::Texture3D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;        rv.Texture3D = { mip, first, slices }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray: rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;   rv.Texture2DArray = { mip, first, slices, d.planeSlice }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: バッファの RTV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocRTV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateRenderTargetView(view.res.Get(), &rv, cpu);
		view.cpu   = cpu;
		view.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		break;
	}
	case GfxViewType::DSV:
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = fmt;
		dv.Flags  = static_cast<D3D12_DSV_FLAGS>(d.dsvFlags);
		switch (d.dimension)
		{
		case GfxViewDimension::Texture1D:        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;        dv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:   dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;   dv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;        dv.Texture2D.MipSlice = mip; break;
		case GfxViewDimension::Texture2DMS:      dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;      break;
		case GfxViewDimension::Texture2DMSArray: dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY; dv.Texture2DMSArray = { first, slices }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray: dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;   dv.Texture2DArray = { mip, first, slices }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: この次元の DSV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocDSV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateDepthStencilView(view.res.Get(), &dv, cpu);
		view.cpu   = cpu;
		// 深度もステンシルも読み取り専用なら DEPTH_READ (シェーダーから同時に読める)
		const bool readOnly = (d.dsvFlags & (GfxDSVFlags::ReadOnlyDepth | GfxDSVFlags::ReadOnlyStencil))
		                   == (GfxDSVFlags::ReadOnlyDepth | GfxDSVFlags::ReadOnlyStencil)
		                   || ((d.dsvFlags & GfxDSVFlags::ReadOnlyDepth) != GfxDSVFlags::None && !hasStencilDXGI(fmt));
		view.state = readOnly ? (D3D12_RESOURCE_STATE_DEPTH_READ | k_ShaderReadState) : D3D12_RESOURCE_STATE_DEPTH_WRITE;
		view.hasStencil = hasStencilDXGI(fmt);
		break;
	}
	}

	const uint64_t id = m_nextId++;
	m_views.emplace(id, std::move(view));
	return ViewHandle{ id };
}

void DX12DeviceImpl::destroyView(ViewHandle h)
{
	auto it = m_views.find(h.id);
	if (it == m_views.end())
		return;
	DX12ViewRes& v = it->second;
	switch (v.type)
	{
	case GfxViewType::RTV: freeRTVDeferred(v.slot);       break;
	case GfxViewType::DSV: freeDSVDeferred(v.slot);       break;
	default:               freeStaticSRVDeferred(v.slot); break;
	}
	deferRelease(std::move(v.res));
	m_views.erase(it);
}

SamplerHandle DX12DeviceImpl::createSamplerState(const GfxSamplerStateDesc* src)
{
	GfxSamplerStateDesc d;
	if (!readVersionedDesc(src, d, "createSamplerState"))
		return {};

	DX12SamplerRes sam;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSampler(sam.slot);
	if (sam.slot == UINT32_MAX)
		return {};

	D3D12_SAMPLER_DESC sd = {};
	sd.Filter         = static_cast<D3D12_FILTER>(d.filter);
	sd.AddressU       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressU);
	sd.AddressV       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressV);
	sd.AddressW       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressW);
	sd.MipLODBias     = d.mipLODBias;
	sd.MaxAnisotropy  = d.maxAnisotropy;
	sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(d.comparisonFunc);
	memcpy(sd.BorderColor, d.borderColor, sizeof(sd.BorderColor));
	sd.MinLOD         = d.minLOD;
	sd.MaxLOD         = d.maxLOD;
	m_device->CreateSampler(&sd, cpu);
	publishStaticSampler(sam.slot);

	const uint64_t id = m_nextId++;
	m_samplers.emplace(id, sam);
	return SamplerHandle{ id };
}

// ============================================================
//  DX12RenderTargetImpl — 実装
// ============================================================

DX12RenderTargetImpl::DX12RenderTargetImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// オフスクリーン RT を作成する
bool DX12RenderTargetImpl::create(
	DX12DeviceImpl*            dev,
	const GfxRenderTargetDesc* desc)
{
	if (desc->sampleCount > 1 && desc->allowUAV)
	{
		fprintf(stderr, "[GfxEngine] createRenderTarget: MSAA (sampleCount=%u) の RT は allowUAV にできません\n", desc->sampleCount);
		return false;
	}
	if (!dev->checkSampleCount(toDXGI(desc->format), desc->sampleCount, 0, "createRenderTarget"))
		return false;

	m_dev         = dev;
	m_desc        = *desc;
	m_desc.sampleCount = desc->sampleCount > 0 ? desc->sampleCount : 1;
	m_width       = desc->width  ? desc->width  : 1;
	m_height      = desc->height ? desc->height : 1;
	m_format      = desc->format;
	m_ownResource = true;

	return allocateResource();
}

// SwapChain バックバッファから RT を作成する
bool DX12RenderTargetImpl::createFromBuffer(
	DX12DeviceImpl* dev,
	ID3D12Resource* buffer,
	PixelFormat     fmt,
	uint32_t        w,
	uint32_t        h)
{
	m_dev         = dev;
	m_ownResource = false;
	m_width       = w;
	m_height      = h;
	m_format      = fmt;

	// ComPtr に入れるため AddRef してから Attach する
	buffer->AddRef();
	m_resource.Attach(buffer);

	m_rtvHandle = dev->allocRTV(m_rtvSlot);
	if (m_rtvSlot == UINT32_MAX)
		return false;
	dev->device()->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtvHandle);

	// バックバッファは PRESENT (= COMMON) 状態で渡される
	dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_PRESENT);
	return true;
}

// テクスチャリソースとビューを作成する
bool DX12RenderTargetImpl::allocateResource()
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width            = m_width;
	rd.Height           = m_height;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.Format           = toDXGI(m_format);
	rd.SampleDesc.Count = m_desc.sampleCount;
	rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	if (m_desc.allowUAV)
		rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_CLEAR_VALUE cv = {};
	cv.Format = toDXGI(m_format);
	memcpy(cv.Color, m_desc.clearColor, sizeof(cv.Color));

	if (FAILED(m_dev->device()->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
			IID_PPV_ARGS(&m_resource))))
		return false;

	setDebugName(m_resource.Get(), m_desc.debugName);

	// RTV (リサイズのたびに新しいスロットを確保する。旧スロットは遅延返却)
	m_rtvHandle = m_dev->allocRTV(m_rtvSlot);
	if (m_rtvSlot == UINT32_MAX)
	{
		m_resource.Reset();
		return false;
	}
	m_dev->device()->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtvHandle);

	// SRV (asTexture 用)
	if (m_desc.allowUAV || m_desc.shaderRead)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = m_dev->allocStaticSRV(m_srvSlot);
		if (m_srvSlot != UINT32_MAX)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format                  = toDXGI(m_format);
			sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (m_desc.sampleCount > 1)
			{
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				sv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
			}

			m_dev->device()->CreateShaderResourceView(m_resource.Get(), &sv, srvCPU);
			m_dev->publishStaticSRV(m_srvSlot);
			m_dev->registerSRVSlot(m_srvSlot, this);
		}
	}

	// リソースは RENDER_TARGET 状態で作成される
	m_dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

// リソースとディスクリプタを解放する
// immediate=false: GPU が使い終わるまで遅延する / true: 呼び出し側が GPU アイドルを保証する
void DX12RenderTargetImpl::freeResource(bool immediate)
{
	if (!m_dev)
		return;

	if (m_srvSlot != UINT32_MAX)
	{
		m_dev->unregisterSRVSlot(m_srvSlot);
		m_dev->freeStaticSRVDeferred(m_srvSlot);
		m_srvSlot = UINT32_MAX;
	}

	ID3D12Resource* raw = m_resource.Get();

	if (immediate)
	{
		if (m_rtvSlot != UINT32_MAX)
			m_dev->freeRTVNow(m_rtvSlot);
		if (raw)
			m_dev->unregisterResourceState(raw);
		m_resource.Reset();
	}
	else
	{
		if (m_rtvSlot != UINT32_MAX)
			m_dev->freeRTVDeferred(m_rtvSlot);
		if (raw)
		{
			DX12DeviceImpl* dev = m_dev;
			m_dev->deferRelease(m_resource, [dev, raw] { dev->unregisterResourceState(raw); });
		}
		m_resource.Reset();
	}
	m_rtvSlot = UINT32_MAX;
}

// 外部から呼ばれる destroy (遅延解放)
void DX12RenderTargetImpl::destroy()
{
	freeResource(false);
}

// バックバッファ用の即時解放 (呼び出し側が GPU アイドルを保証する)
void DX12RenderTargetImpl::destroyImmediate()
{
	freeResource(true);
}

// サイズを変更してリソースを再作成する
// asTexture() のハンドルは変わるため、リサイズ後は取得し直すこと
bool DX12RenderTargetImpl::resize(uint32_t w, uint32_t h)
{
	if (!m_ownResource || (w == m_width && h == m_height) || w == 0 || h == 0)
		return true;

	m_width       = w;
	m_height      = h;
	m_desc.width  = w;
	m_desc.height = h;

	freeResource(false);
	return allocateResource();
}

// SRV として参照するハンドルを返す
// shaderRead / allowUAV を指定して作成した RT のみ有効
TextureHandle DX12RenderTargetImpl::asTexture()
{
	if (m_srvSlot == UINT32_MAX)
		return {};
	// 上位ビットに RT フラグを立てて createTexture の id と区別する
	return { DX12DeviceImpl::RT_TEXTURE_FLAG | static_cast<uint64_t>(m_srvSlot) };
}

// ============================================================
//  DX12DepthStencilImpl — 実装
// ============================================================

DX12DepthStencilImpl::DX12DepthStencilImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// DS リソースと DSV を作成する
bool DX12DepthStencilImpl::create(
	DX12DeviceImpl*            dev,
	const GfxDepthStencilDesc* desc)
{
	if (!dev->checkSampleCount(toDXGI(desc->format), desc->sampleCount, 0, "createDepthStencil"))
		return false;

	m_dev    = dev;
	m_desc   = *desc;
	m_desc.sampleCount = desc->sampleCount > 0 ? desc->sampleCount : 1;
	m_width  = desc->width  ? desc->width  : 1;
	m_height = desc->height ? desc->height : 1;
	m_format = desc->format;

	return allocateResource();
}

// テクスチャリソースと DSV / SRV を作成する
bool DX12DepthStencilImpl::allocateResource()
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width            = m_width;
	rd.Height           = m_height;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.SampleDesc.Count = m_desc.sampleCount;
	rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// SRV としても使うため TYPELESS フォーマットでリソースを作成する
	DXGI_FORMAT resourceFmt = toDXGI(m_format);
	DXGI_FORMAT dsvFmt      = toDXGI(m_format);
	DXGI_FORMAT srvFmt      = DXGI_FORMAT_UNKNOWN;
	switch (m_format)
	{
	case PixelFormat::D32_FLOAT:
		resourceFmt = DXGI_FORMAT_R32_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D32_FLOAT;
		srvFmt      = DXGI_FORMAT_R32_FLOAT;
		break;
	case PixelFormat::D24_UNORM_S8_UINT:
		resourceFmt = DXGI_FORMAT_R24G8_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D24_UNORM_S8_UINT;
		srvFmt      = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		break;
	case PixelFormat::D16_UNORM:
		resourceFmt = DXGI_FORMAT_R16_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D16_UNORM;
		srvFmt      = DXGI_FORMAT_R16_UNORM;
		break;
	default:
		break;
	}
	rd.Format = resourceFmt;

	D3D12_CLEAR_VALUE cv = {};
	cv.Format                  = dsvFmt;
	cv.DepthStencil.Depth      = m_desc.clearDepth;
	cv.DepthStencil.Stencil    = m_desc.clearStencil;

	if (FAILED(m_dev->device()->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
			IID_PPV_ARGS(&m_resource))))
		return false;

	setDebugName(m_resource.Get(), m_desc.debugName);

	// DSV (リサイズのたびに新しいスロットを確保する。旧スロットは遅延返却)
	m_dsvHandle = m_dev->allocDSV(m_dsvSlot);
	if (m_dsvSlot == UINT32_MAX)
	{
		m_resource.Reset();
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {};
	dsvd.Format        = dsvFmt;
	dsvd.ViewDimension = m_desc.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	m_dev->device()->CreateDepthStencilView(m_resource.Get(), &dsvd, m_dsvHandle);

	// Depth リソースから SRV を作成してシェーダーから読めるようにする
	if (srvFmt != DXGI_FORMAT_UNKNOWN)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = m_dev->allocStaticSRV(m_srvSlot);
		if (m_srvSlot != UINT32_MAX)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format                    = srvFmt;
			sv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (m_desc.sampleCount > 1)
			{
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				sv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
			}

			m_dev->device()->CreateShaderResourceView(m_resource.Get(), &sv, srvCPU);
			m_dev->publishStaticSRV(m_srvSlot);
			m_dev->registerDSSRVSlot(m_srvSlot, this);
		}
	}

	m_dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	return true;
}

// リソースとディスクリプタを遅延解放する
void DX12DepthStencilImpl::freeResource()
{
	if (!m_dev)
		return;

	if (m_dsvSlot != UINT32_MAX)
	{
		m_dev->freeDSVDeferred(m_dsvSlot);
		m_dsvSlot = UINT32_MAX;
	}
	if (m_srvSlot != UINT32_MAX)
	{
		m_dev->unregisterDSSRVSlot(m_srvSlot);
		m_dev->freeStaticSRVDeferred(m_srvSlot);
		m_srvSlot = UINT32_MAX;
	}
	if (ID3D12Resource* raw = m_resource.Get())
	{
		DX12DeviceImpl* dev = m_dev;
		m_dev->deferRelease(m_resource, [dev, raw] { dev->unregisterResourceState(raw); });
	}
	m_resource.Reset();
}

// 外部から呼ばれる destroy
void DX12DepthStencilImpl::destroy()
{
	freeResource();
}

// サイズを変更してリソースを再作成する
bool DX12DepthStencilImpl::resize(uint32_t w, uint32_t h)
{
	if ((w == m_width && h == m_height) || w == 0 || h == 0)
		return true;

	m_width       = w;
	m_height      = h;
	m_desc.width  = w;
	m_desc.height = h;

	freeResource();
	return allocateResource();
}

// デプスを SRV として参照するハンドルを返す
TextureHandle DX12DepthStencilImpl::asTexture()
{
	if (m_srvSlot == UINT32_MAX)
		return {};
	return { DX12DeviceImpl::RT_TEXTURE_FLAG | static_cast<uint64_t>(m_srvSlot) };
}
