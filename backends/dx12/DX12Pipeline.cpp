// ============================================================
//  backends/dx12/DX12Pipeline.cpp
//  グラフィックスパイプライン / バインディングレイアウト (v1 と v2)、旧経路の PSO キャッシュ
// ============================================================

#include "DX12Device.h"

// ============================================================
//  DX12PipelineImpl — 実装
// ============================================================

// ---- ABI v2 の列挙値と D3D12 の値が一致することを確認する ----
#define GFX_CHECK_D3D(gfx, d3d) static_assert(static_cast<uint32_t>(gfx) == static_cast<uint32_t>(d3d), #gfx " != " #d3d);
GFX_CHECK_D3D(GfxFormat::R32G32B32A32_Float,          DXGI_FORMAT_R32G32B32A32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32G32B32_Float,             DXGI_FORMAT_R32G32B32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R16G16B16A16_Float,          DXGI_FORMAT_R16G16B16A16_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32G32_Float,                DXGI_FORMAT_R32G32_FLOAT)
GFX_CHECK_D3D(GfxFormat::D32_Float_S8X24_Uint,        DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
GFX_CHECK_D3D(GfxFormat::R10G10B10A2_Unorm,           DXGI_FORMAT_R10G10B10A2_UNORM)
GFX_CHECK_D3D(GfxFormat::R11G11B10_Float,             DXGI_FORMAT_R11G11B10_FLOAT)
GFX_CHECK_D3D(GfxFormat::R8G8B8A8_Unorm,              DXGI_FORMAT_R8G8B8A8_UNORM)
GFX_CHECK_D3D(GfxFormat::R8G8B8A8_Sint,               DXGI_FORMAT_R8G8B8A8_SINT)
GFX_CHECK_D3D(GfxFormat::R16G16_Sint,                 DXGI_FORMAT_R16G16_SINT)
GFX_CHECK_D3D(GfxFormat::D32_Float,                   DXGI_FORMAT_D32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32_Sint,                    DXGI_FORMAT_R32_SINT)
GFX_CHECK_D3D(GfxFormat::D24_Unorm_S8_Uint,           DXGI_FORMAT_D24_UNORM_S8_UINT)
GFX_CHECK_D3D(GfxFormat::R8G8_Sint,                   DXGI_FORMAT_R8G8_SINT)
GFX_CHECK_D3D(GfxFormat::D16_Unorm,                   DXGI_FORMAT_D16_UNORM)
GFX_CHECK_D3D(GfxFormat::R16_Sint,                    DXGI_FORMAT_R16_SINT)
GFX_CHECK_D3D(GfxFormat::R8_Sint,                     DXGI_FORMAT_R8_SINT)
GFX_CHECK_D3D(GfxFormat::B8G8R8A8_Unorm,              DXGI_FORMAT_B8G8R8A8_UNORM)
GFX_CHECK_D3D(GfxFormat::B8G8R8A8_Unorm_sRGB,         DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
GFX_CHECK_D3D(GfxBlend::Zero,                         D3D12_BLEND_ZERO)
GFX_CHECK_D3D(GfxBlend::SrcAlphaSat,                  D3D12_BLEND_SRC_ALPHA_SAT)
GFX_CHECK_D3D(GfxBlend::BlendFactor,                  D3D12_BLEND_BLEND_FACTOR)
GFX_CHECK_D3D(GfxBlend::InvSrc1Alpha,                 D3D12_BLEND_INV_SRC1_ALPHA)
GFX_CHECK_D3D(GfxBlendOp::Add,                        D3D12_BLEND_OP_ADD)
GFX_CHECK_D3D(GfxBlendOp::Max,                        D3D12_BLEND_OP_MAX)
GFX_CHECK_D3D(GfxLogicOp::Clear,                      D3D12_LOGIC_OP_CLEAR)
GFX_CHECK_D3D(GfxLogicOp::Noop,                       D3D12_LOGIC_OP_NOOP)
GFX_CHECK_D3D(GfxLogicOp::OrInverted,                 D3D12_LOGIC_OP_OR_INVERTED)
GFX_CHECK_D3D(GfxColorWriteMask::Red,                 D3D12_COLOR_WRITE_ENABLE_RED)
GFX_CHECK_D3D(GfxColorWriteMask::Alpha,               D3D12_COLOR_WRITE_ENABLE_ALPHA)
GFX_CHECK_D3D(GfxColorWriteMask::All,                 D3D12_COLOR_WRITE_ENABLE_ALL)
GFX_CHECK_D3D(GfxFillMode::Wireframe,                 D3D12_FILL_MODE_WIREFRAME)
GFX_CHECK_D3D(GfxFillMode::Solid,                     D3D12_FILL_MODE_SOLID)
GFX_CHECK_D3D(GfxCullMode::None,                      D3D12_CULL_MODE_NONE)
GFX_CHECK_D3D(GfxCullMode::Back,                      D3D12_CULL_MODE_BACK)
GFX_CHECK_D3D(GfxConservativeRaster::On,              D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON)
GFX_CHECK_D3D(GfxCompareFunc::Never,                  D3D12_COMPARISON_FUNC_NEVER)
GFX_CHECK_D3D(GfxCompareFunc::LessEqual,              D3D12_COMPARISON_FUNC_LESS_EQUAL)
GFX_CHECK_D3D(GfxCompareFunc::Always,                 D3D12_COMPARISON_FUNC_ALWAYS)
GFX_CHECK_D3D(GfxDepthWriteMask::All,                 D3D12_DEPTH_WRITE_MASK_ALL)
GFX_CHECK_D3D(GfxStencilOp::Keep,                     D3D12_STENCIL_OP_KEEP)
GFX_CHECK_D3D(GfxStencilOp::Decr,                     D3D12_STENCIL_OP_DECR)
GFX_CHECK_D3D(GfxInputClassification::PerInstance,    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA)
GFX_CHECK_D3D(GfxIndexBufferStripCut::Value0xFFFFFFFF, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)
GFX_CHECK_D3D(GfxPrimitiveTopologyType::Line,         D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
GFX_CHECK_D3D(GfxPrimitiveTopologyType::Patch,        D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
GFX_CHECK_D3D(GfxPrimitiveTopology::LineList,         D3D_PRIMITIVE_TOPOLOGY_LINELIST)
GFX_CHECK_D3D(GfxPrimitiveTopology::TriangleStrip,    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
GFX_CHECK_D3D(GfxPrimitiveTopology::TriangleStripAdj, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ)
GFX_CHECK_D3D(GfxPrimitiveTopology::PatchList1,       D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
GFX_CHECK_D3D(GfxPrimitiveTopology::PatchList32,      D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
GFX_CHECK_D3D(GfxPipelineStateFlags::ToolDebug,       D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG)
GFX_CHECK_D3D(GfxDescriptorRangeType::Sampler,        D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
GFX_CHECK_D3D(GfxDescriptorRangeType::CBV,            D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DescriptorsVolatile, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DataStatic,    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DescriptorsStaticKeepingBufferBoundsChecks, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS)
GFX_CHECK_D3D(GfxRootParameterType::Constants32Bit,   D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
GFX_CHECK_D3D(GfxRootParameterType::UAV,              D3D12_ROOT_PARAMETER_TYPE_UAV)
GFX_CHECK_D3D(GfxShaderVisibility::Pixel,             D3D12_SHADER_VISIBILITY_PIXEL)
GFX_CHECK_D3D(GfxShaderVisibility::Mesh,              D3D12_SHADER_VISIBILITY_MESH)
GFX_CHECK_D3D(GfxRootDescriptorFlags::DataStatic,     D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC)
GFX_CHECK_D3D(GfxRootSignatureFlags::AllowInputAssemblerInputLayout, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
GFX_CHECK_D3D(GfxRootSignatureFlags::DenyMeshShaderRootAccess,       D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS)
GFX_CHECK_D3D(GfxRootSignatureFlags::SamplerHeapDirectlyIndexed,     D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)
GFX_CHECK_D3D(GfxFilter::MinMagMipLinear,             D3D12_FILTER_MIN_MAG_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::Anisotropic,                 D3D12_FILTER_ANISOTROPIC)
GFX_CHECK_D3D(GfxFilter::ComparisonMinMagMipLinear,   D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::ComparisonAnisotropic,       D3D12_FILTER_COMPARISON_ANISOTROPIC)
GFX_CHECK_D3D(GfxFilter::MinimumMinLinearMagPointMipLinear, D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::MaximumAnisotropic,          D3D12_FILTER_MAXIMUM_ANISOTROPIC)
GFX_CHECK_D3D(GfxTextureAddressMode::MirrorOnce,      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE)
GFX_CHECK_D3D(GfxStaticBorderColor::OpaqueWhite,      D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE)
#undef GFX_CHECK_D3D

static_assert(GFX_MAX_RENDER_TARGETS == D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, "RT count");
static_assert(GFX_APPEND_ALIGNED_ELEMENT == D3D12_APPEND_ALIGNED_ELEMENT, "append aligned");
static_assert(GFX_DESCRIPTOR_RANGE_OFFSET_APPEND == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND, "range append");

// バインディングレイアウトから、既存のバインドコマンドとルートパラメータの対応を求める
static DX12BindingSlots computeBindingSlots(const GfxBindingLayoutDesc& d)
{
	DX12BindingSlots out;
	out.numParameters = d.numParameters;
	for (uint32_t i = 0; i < d.numParameters; ++i)
	{
		const GfxRootParameter& p = d.parameters[i];
		out.paramTypes[i] = p.parameterType;
		if (p.parameterType == GfxRootParameterType::DescriptorTable)
		{
			bool     allSRV     = p.numRanges > 0;
			bool     allSampler = p.numRanges > 0;
			uint32_t cursor     = 0;
			uint32_t total      = 0;
			for (uint32_t r = p.firstRange; r < p.firstRange + p.numRanges; ++r)
			{
				const GfxDescriptorRange& range = d.ranges[r];
				allSRV     = allSRV     && range.rangeType == GfxDescriptorRangeType::SRV;
				allSampler = allSampler && range.rangeType == GfxDescriptorRangeType::Sampler;

				const uint32_t first = (range.offsetInDescriptorsFromTableStart == GFX_DESCRIPTOR_RANGE_OFFSET_APPEND)
					? cursor : range.offsetInDescriptorsFromTableStart;
				// 上限なしのレンジはバインドした数だけ埋める (テーブルの大きさに含めない)
				cursor = (range.numDescriptors == GFX_UNBOUNDED_DESCRIPTOR_COUNT) ? first : first + range.numDescriptors;
				total  = std::max(total, cursor);
			}
			if (allSRV && out.srvTableSlot == UINT32_MAX)
			{
				out.srvTableSlot  = i;
				out.srvTableCount = total;
			}
			else if (allSampler && out.samplerTableSlot == UINT32_MAX)
			{
				out.samplerTableSlot  = i;
				out.samplerTableCount = total;
			}
		}
		else if (p.parameterType == GfxRootParameterType::CBV
			  && p.registerSpace == 0
			  && out.cbvCount < DX12BindingSlots::MAX_CBV)
		{
			out.cbvBindRegisters[out.cbvCount] = p.shaderRegister;
			out.cbvRootSlots    [out.cbvCount] = i;
			++out.cbvCount;
		}
	}
	return out;
}

// GfxBindingLayoutDesc → D3D12 ルートシグネチャ (バージョン 1.1)
BindingLayoutHandle DX12DeviceImpl::createBindingLayout(const GfxBindingLayoutDesc* src)
{
	GfxBindingLayoutDesc d;
	if (!readVersionedDesc(src, d, "createBindingLayout"))
		return {};

	if (d.numParameters > GFX_MAX_ROOT_PARAMETERS || d.numRanges > GFX_MAX_DESCRIPTOR_RANGES
	 || d.numStaticSamplers > GFX_MAX_STATIC_SAMPLERS)
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: パラメータ / レンジ / 静的サンプラーの数が上限を超えています\n");
		return {};
	}

	D3D12_DESCRIPTOR_RANGE1    ranges  [GFX_MAX_DESCRIPTOR_RANGES] = {};
	D3D12_ROOT_PARAMETER1      params  [GFX_MAX_ROOT_PARAMETERS]   = {};
	D3D12_STATIC_SAMPLER_DESC  samplers[GFX_MAX_STATIC_SAMPLERS]   = {};

	for (uint32_t i = 0; i < d.numRanges; ++i)
	{
		const GfxDescriptorRange& r = d.ranges[i];
		ranges[i].RangeType                         = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(r.rangeType);
		ranges[i].NumDescriptors                    = r.numDescriptors;
		ranges[i].BaseShaderRegister                = r.baseShaderRegister;
		ranges[i].RegisterSpace                     = r.registerSpace;
		ranges[i].Flags                             = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(r.flags);
		ranges[i].OffsetInDescriptorsFromTableStart = r.offsetInDescriptorsFromTableStart;
	}

	for (uint32_t i = 0; i < d.numParameters; ++i)
	{
		const GfxRootParameter& p = d.parameters[i];
		D3D12_ROOT_PARAMETER1&  o = params[i];
		o.ParameterType    = static_cast<D3D12_ROOT_PARAMETER_TYPE>(p.parameterType);
		o.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(p.shaderVisibility);
		switch (p.parameterType)
		{
		case GfxRootParameterType::DescriptorTable:
			if (p.numRanges == 0 || p.firstRange + p.numRanges > d.numRanges)
			{
				fprintf(stderr, "[GfxEngine] createBindingLayout: parameters[%u] のレンジ [%u, %u) が ranges (%u 個) の範囲外です\n",
					i, p.firstRange, p.firstRange + p.numRanges, d.numRanges);
				return {};
			}
			o.DescriptorTable.NumDescriptorRanges = p.numRanges;
			o.DescriptorTable.pDescriptorRanges   = &ranges[p.firstRange];
			break;
		case GfxRootParameterType::Constants32Bit:
			o.Constants.ShaderRegister = p.shaderRegister;
			o.Constants.RegisterSpace  = p.registerSpace;
			o.Constants.Num32BitValues = p.num32BitValues;
			break;
		case GfxRootParameterType::CBV:
		case GfxRootParameterType::SRV:
		case GfxRootParameterType::UAV:
			o.Descriptor.ShaderRegister = p.shaderRegister;
			o.Descriptor.RegisterSpace  = p.registerSpace;
			o.Descriptor.Flags          = static_cast<D3D12_ROOT_DESCRIPTOR_FLAGS>(p.descriptorFlags);
			break;
		default:
			fprintf(stderr, "[GfxEngine] createBindingLayout: parameters[%u] の parameterType (%u) が不正です\n",
				i, static_cast<uint32_t>(p.parameterType));
			return {};
		}
	}

	for (uint32_t i = 0; i < d.numStaticSamplers; ++i)
	{
		const GfxStaticSamplerDesc& s = d.staticSamplers[i];
		D3D12_STATIC_SAMPLER_DESC&  o = samplers[i];
		o.Filter           = static_cast<D3D12_FILTER>(s.filter);
		o.AddressU         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressU);
		o.AddressV         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressV);
		o.AddressW         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressW);
		o.MipLODBias       = s.mipLODBias;
		o.MaxAnisotropy    = s.maxAnisotropy;
		o.ComparisonFunc   = static_cast<D3D12_COMPARISON_FUNC>(s.comparisonFunc);
		o.BorderColor      = static_cast<D3D12_STATIC_BORDER_COLOR>(s.borderColor);
		o.MinLOD           = s.minLOD;
		o.MaxLOD           = s.maxLOD;
		o.ShaderRegister   = s.shaderRegister;
		o.RegisterSpace    = s.registerSpace;
		o.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(s.shaderVisibility);
	}

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsD = {};
	rsD.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsD.Desc_1_1.NumParameters     = d.numParameters;
	rsD.Desc_1_1.pParameters       = d.numParameters > 0 ? params : nullptr;
	rsD.Desc_1_1.NumStaticSamplers = d.numStaticSamplers;
	rsD.Desc_1_1.pStaticSamplers   = d.numStaticSamplers > 0 ? samplers : nullptr;
	rsD.Desc_1_1.Flags             = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(d.flags);

	ComPtr<ID3DBlob> sigBlob, errBlob;
	if (FAILED(D3D12SerializeVersionedRootSignature(&rsD, &sigBlob, &errBlob)))
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: シリアライズに失敗しました: %s\n",
			errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "(no message)");
		return {};
	}

	DX12BindingLayoutRes res;
	if (FAILED(m_device->CreateRootSignature(
			0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&res.rootSig))))
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: CreateRootSignature に失敗しました\n");
		return {};
	}
	setDebugName(res.rootSig.Get(), d.debugName);
	res.slots = computeBindingSlots(d);

	const uint64_t id = m_nextId++;
	m_bindingLayouts.emplace(id, std::move(res));
	return BindingLayoutHandle{ id };
}

// ルートシグネチャは記録済みのコマンドリストが参照しているため遅延解放する。
// それを使って作ったパイプラインは自分で参照を持つので、そのまま使い続けられる。
void DX12DeviceImpl::destroyBindingLayout(BindingLayoutHandle h)
{
	auto it = m_bindingLayouts.find(h.id);
	if (it == m_bindingLayouts.end())
		return;
	deferRelease(std::move(it->second.rootSig));
	m_bindingLayouts.erase(it);
}

// IPipelineVTable の vtable 定義
static const IPipelineVTable k_PipelineVTable =
{
	[](void* s)
	{
		auto* impl = static_cast<DX12PipelineImpl*>(s);
		impl->device()->destroyPipelineImpl(impl);
	}
};

const IPipelineVTable* DX12PipelineImpl::getVTable()
{
	return &k_PipelineVTable;
}

// GfxPipelineDesc から PSO を作成する
DX12PipelineImpl::DX12PipelineImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

namespace
{
	void reportPipelineError(DX12DeviceImpl* dev, const char* name, const std::string& what)
	{
		const std::string text = "pipeline (" + std::string(name && *name ? name : "unnamed") + "): " + what;
		dev->emitDebugMessage(GfxMessageSeverity::Error, GFX_MESSAGE_ID_PIPELINE, text.c_str());
	}
}

void DX12PipelineImpl::keepDebugName(const char*& name)
{
	if (name)
		m_debugName = name;
	name = m_debugName.empty() ? nullptr : m_debugName.c_str();
}

bool DX12PipelineImpl::create(
	DX12DeviceImpl*                dev,
	const GfxGraphicsPipelineDesc& desc,
	const DX12BindingLayoutRes&    layout)
{
	m_dev = dev;
	ComPtr<ID3D12PipelineState> pso = buildGraphicsPSO(dev, desc, layout.rootSig.Get());
	if (!pso)
		return false;
	replace(desc, layout, std::move(pso));
	return true;
}

ComPtr<ID3D12PipelineState> DX12PipelineImpl::replace(
	const GfxGraphicsPipelineDesc& desc,
	const DX12BindingLayoutRes&    layout,
	ComPtr<ID3D12PipelineState>    pso)
{
	m_isCompute    = false;
	m_rootSig      = layout.rootSig;
	m_slots        = layout.slots;
	m_topology     = static_cast<D3D_PRIMITIVE_TOPOLOGY>(desc.primitiveTopology);
	m_graphicsDesc = desc;
	keepDebugName(m_graphicsDesc.debugName);
	return swapPSO(std::move(pso));
}

ComPtr<ID3D12PipelineState> DX12PipelineImpl::replace(
	const GfxComputePipelineDesc& desc,
	const DX12BindingLayoutRes&   layout,
	ComPtr<ID3D12PipelineState>   pso)
{
	m_isCompute   = true;
	m_rootSig     = layout.rootSig;
	m_slots       = layout.slots;
	m_computeDesc = desc;
	keepDebugName(m_computeDesc.debugName);
	return swapPSO(std::move(pso));
}

bool DX12PipelineImpl::usesShader(uint64_t id) const
{
	if (m_isCompute)
		return m_computeDesc.cs.id == id;
	const GfxGraphicsPipelineDesc& d = m_graphicsDesc;
	return d.vs.id == id || d.ps.id == id || d.ds.id == id || d.hs.id == id || d.gs.id == id;
}

ComPtr<ID3D12PipelineState> DX12PipelineImpl::rebuildPSO() const
{
	return m_isCompute
		? buildComputePSO (m_dev, m_computeDesc,  m_rootSig.Get())
		: buildGraphicsPSO(m_dev, m_graphicsDesc, m_rootSig.Get());
}

ComPtr<ID3D12PipelineState> DX12PipelineImpl::buildGraphicsPSO(
	DX12DeviceImpl*                dev,
	const GfxGraphicsPipelineDesc& desc,
	ID3D12RootSignature*           rootSig)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = rootSig;

	// シェーダー (VS は必須。PS は depth-only パスでは不要)
	auto setShader = [&](ShaderHandle h, D3D12_SHADER_BYTECODE& out, const char* stage) -> bool
	{
		if (!h.valid())
			return true;
		const DX12ShadRes* sh = dev->getShadRes(h.id);
		if (!sh || sh->bytecode.empty())
		{
			reportPipelineError(dev, desc.debugName, std::string(stage) + " のシェーダーハンドルが無効です");
			return false;
		}
		out = { sh->bytecode.data(), sh->bytecode.size() };
		return true;
	};
	if (!desc.vs.valid())
	{
		reportPipelineError(dev, desc.debugName, "VS は必須です");
		return nullptr;
	}
	if (!setShader(desc.vs, psoDesc.VS, "VS") || !setShader(desc.ps, psoDesc.PS, "PS")
	 || !setShader(desc.ds, psoDesc.DS, "DS") || !setShader(desc.hs, psoDesc.HS, "HS")
	 || !setShader(desc.gs, psoDesc.GS, "GS"))
		return nullptr;

	// BlendState
	psoDesc.BlendState.AlphaToCoverageEnable  = desc.blendState.alphaToCoverageEnable  ? TRUE : FALSE;
	psoDesc.BlendState.IndependentBlendEnable = desc.blendState.independentBlendEnable ? TRUE : FALSE;
	for (uint32_t i = 0; i < GFX_MAX_RENDER_TARGETS; ++i)
	{
		const GfxRenderTargetBlendDesc&  b = desc.blendState.renderTarget[i];
		D3D12_RENDER_TARGET_BLEND_DESC&  o = psoDesc.BlendState.RenderTarget[i];
		o.BlendEnable           = b.blendEnable   ? TRUE : FALSE;
		o.LogicOpEnable         = b.logicOpEnable ? TRUE : FALSE;
		o.SrcBlend              = static_cast<D3D12_BLEND>(b.srcBlend);
		o.DestBlend             = static_cast<D3D12_BLEND>(b.destBlend);
		o.BlendOp               = static_cast<D3D12_BLEND_OP>(b.blendOp);
		o.SrcBlendAlpha         = static_cast<D3D12_BLEND>(b.srcBlendAlpha);
		o.DestBlendAlpha        = static_cast<D3D12_BLEND>(b.destBlendAlpha);
		o.BlendOpAlpha          = static_cast<D3D12_BLEND_OP>(b.blendOpAlpha);
		o.LogicOp               = static_cast<D3D12_LOGIC_OP>(b.logicOp);
		o.RenderTargetWriteMask = static_cast<UINT8>(b.renderTargetWriteMask);
	}
	psoDesc.SampleMask = desc.sampleMask;

	// RasterizerState
	{
		const GfxRasterizerDesc& r = desc.rasterizerState;
		D3D12_RASTERIZER_DESC&   o = psoDesc.RasterizerState;
		o.FillMode              = static_cast<D3D12_FILL_MODE>(r.fillMode);
		o.CullMode              = static_cast<D3D12_CULL_MODE>(r.cullMode);
		o.FrontCounterClockwise = r.frontCounterClockwise ? TRUE : FALSE;
		o.DepthBias             = r.depthBias;
		o.DepthBiasClamp        = r.depthBiasClamp;
		o.SlopeScaledDepthBias  = r.slopeScaledDepthBias;
		o.DepthClipEnable       = r.depthClipEnable       ? TRUE : FALSE;
		o.MultisampleEnable     = r.multisampleEnable     ? TRUE : FALSE;
		o.AntialiasedLineEnable = r.antialiasedLineEnable ? TRUE : FALSE;
		o.ForcedSampleCount     = r.forcedSampleCount;
		o.ConservativeRaster    = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(r.conservativeRaster);
	}

	// DepthStencilState
	{
		const GfxDepthStencilStateDesc& ds = desc.depthStencilState;
		D3D12_DEPTH_STENCIL_DESC&       o  = psoDesc.DepthStencilState;
		auto toStencilOp = [](const GfxStencilOpDesc& s)
		{
			D3D12_DEPTH_STENCILOP_DESC r = {};
			r.StencilFailOp      = static_cast<D3D12_STENCIL_OP>(s.stencilFailOp);
			r.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(s.stencilDepthFailOp);
			r.StencilPassOp      = static_cast<D3D12_STENCIL_OP>(s.stencilPassOp);
			r.StencilFunc        = static_cast<D3D12_COMPARISON_FUNC>(s.stencilFunc);
			return r;
		};
		o.DepthEnable      = ds.depthEnable   ? TRUE : FALSE;
		o.DepthWriteMask   = static_cast<D3D12_DEPTH_WRITE_MASK>(ds.depthWriteMask);
		o.DepthFunc        = static_cast<D3D12_COMPARISON_FUNC>(ds.depthFunc);
		o.StencilEnable    = ds.stencilEnable ? TRUE : FALSE;
		o.StencilReadMask  = ds.stencilReadMask;
		o.StencilWriteMask = ds.stencilWriteMask;
		o.FrontFace        = toStencilOp(ds.frontFace);
		o.BackFace         = toStencilOp(ds.backFace);
	}

	// InputLayout (semanticName は desc 内の文字列を指す。desc は PSO 作成まで有効)
	D3D12_INPUT_ELEMENT_DESC inputElems[GFX_MAX_INPUT_ELEMENTS] = {};
	const uint32_t inputCount = std::min(desc.inputLayout.elementCount, GFX_MAX_INPUT_ELEMENTS);
	for (uint32_t i = 0; i < inputCount; ++i)
	{
		const GfxInputElementDesc& e = desc.inputLayout.elements[i];
		inputElems[i].SemanticName         = e.semanticName;
		inputElems[i].SemanticIndex        = e.semanticIndex;
		inputElems[i].Format               = static_cast<DXGI_FORMAT>(e.format);
		inputElems[i].InputSlot            = e.inputSlot;
		inputElems[i].AlignedByteOffset    = e.alignedByteOffset;
		inputElems[i].InputSlotClass       = static_cast<D3D12_INPUT_CLASSIFICATION>(e.inputSlotClass);
		inputElems[i].InstanceDataStepRate = e.instanceDataStepRate;
	}
	if (inputCount > 0)
		psoDesc.InputLayout = { inputElems, inputCount };

	psoDesc.IBStripCutValue       = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(desc.ibStripCutValue);
	psoDesc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(desc.primitiveTopologyType);

	psoDesc.NumRenderTargets = std::min(desc.numRenderTargets, GFX_MAX_RENDER_TARGETS);
	for (uint32_t i = 0; i < psoDesc.NumRenderTargets; ++i)
		psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(desc.rtvFormats[i]);
	psoDesc.DSVFormat  = static_cast<DXGI_FORMAT>(desc.dsvFormat);
	psoDesc.SampleDesc = { desc.sampleCount, desc.sampleQuality };
	psoDesc.NodeMask   = desc.nodeMask;
	psoDesc.Flags      = static_cast<D3D12_PIPELINE_STATE_FLAGS>(desc.flags);

	ComPtr<ID3D12PipelineState> pso;
	if (FAILED(dev->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))))
	{
		reportPipelineError(dev, desc.debugName, "CreateGraphicsPipelineState に失敗しました (シェーダーのシグネチャ / フォーマット / ステートの組み合わせを確認してください)");
		return nullptr;
	}
	setDebugName(pso.Get(), desc.debugName);
	return pso;
}

// v2: GfxGraphicsPipelineDesc から PSO を作成し IPipeline* を PipelineHandle に格納して返す
PipelineHandle DX12DeviceImpl::createGraphicsPipeline(const GfxGraphicsPipelineDesc* src)
{
	GfxGraphicsPipelineDesc d;
	if (!readVersionedDesc(src, d, "createGraphicsPipeline"))
		return {};

	auto layoutIt = m_bindingLayouts.find(d.bindingLayout.id);
	if (layoutIt == m_bindingLayouts.end())
	{
		fprintf(stderr, "[GfxEngine] createGraphicsPipeline: bindingLayout が無効です (%s)\n",
			d.debugName ? d.debugName : "unnamed");
		return {};
	}

	// セマンティクス名は必ず終端させる
	for (auto& e : d.inputLayout.elements)
		e.semanticName[GFX_MAX_SEMANTIC_NAME - 1] = '\0';

	auto* impl = new DX12PipelineImpl();
	if (!impl->create(this, d, layoutIt->second))
	{
		delete impl;
		return {};
	}

	// PipelineHandle.id に IPipeline* (impl が所有するラッパー) を直接格納する
	// bindPipeline が IPipeline* を直接受け取る設計と整合する
	const PipelineHandle h{ reinterpret_cast<uint64_t>(impl->iface()) };
	m_pipelines.emplace(h.id, impl);
	return h;
}

// v1 互換: GfxPipelineDesc を v2 (バインディングレイアウト + パイプライン) に変換して作成する
PipelineHandle DX12DeviceImpl::createPipeline(const GfxPipelineDesc* desc)
{
	if (!desc)
		return {};

	// paramCount == 0 は設定漏れとして扱いエラーを返す
	// GfxRootSignatureDesc::defaultLayout() または明示的な add() を使うこと
	if (desc->rootSignature.paramCount == 0)
	{
		fprintf(stderr, "[GfxEngine] createPipeline: rootSignature.paramCount == 0. "
			"Use GfxRootSignatureDesc::defaultLayout() or add() explicitly.\n");
		return {};
	}

	GfxBindingLayoutDesc layoutDesc = gfxConvertRootSignature(desc->rootSignature);
	layoutDesc.debugName = desc->debugName;
	const BindingLayoutHandle layout = createBindingLayout(&layoutDesc);
	if (!layout.valid())
		return {};

	GfxGraphicsPipelineDesc pipeDesc = gfxConvertPipelineDesc(*desc);
	pipeDesc.bindingLayout = layout;
	const PipelineHandle h = createGraphicsPipeline(&pipeDesc);

	// パイプラインがルートシグネチャの参照を持つので、レイアウトのハンドルはここで手放す
	destroyBindingLayout(layout);
	return h;
}

void DX12DeviceImpl::destroyPipeline(PipelineHandle h)
{
	if (!h.valid()) return;
	auto* p = reinterpret_cast<IPipeline*>(h.id);
	destroyPipelineImpl(static_cast<DX12PipelineImpl*>(p->self));
}

// PSO / ルートシグネチャは記録済み・実行中のコマンドリストが参照しているため遅延解放する。
// impl (とラッパー) は CPU 側の情報だけなので即時に解放してよい。
void DX12DeviceImpl::destroyPipelineImpl(DX12PipelineImpl* impl)
{
	if (!impl) return;
	m_pipelines.erase(reinterpret_cast<uint64_t>(impl->iface()));
	deferRelease(impl->takePSO());
	deferRelease(impl->takeRootSig());
	delete impl;
}

ID3D12PipelineState* DX12DeviceImpl::getOrCreatePSO(
	uint64_t             vsId,
	uint64_t             psId,
	const DXGI_FORMAT*   rtvFmts,
	uint32_t             rtvCount,
	DXGI_FORMAT          dsvFmt)
{
	// VS のレイアウト情報を取得してキーに含める
	const DX12ShadRes* vs = getShadRes(vsId);
	const DX12ShadRes* ps = getShadRes(psId);

	// layout は GfxPipelineDesc に移動済みのため layoutHash は 0 固定
	const uint64_t layoutHash = 0;

	DX12PSOKey key;
	key.vsId       = vsId;
	key.psId       = psId;
	key.rtvCount   = rtvCount;
	key.dsvFmt     = dsvFmt;
	key.layoutHash = layoutHash;
	for (uint32_t i = 0; i < rtvCount; ++i)
		key.rtvFmts[i] = rtvFmts[i];

	auto it = m_psoCache.find(key);
	if (it != m_psoCache.end())
		return it->second.Get();

	const DXGI_FORMAT rtvFmt = rtvCount > 0 ? rtvFmts[0] : DXGI_FORMAT_UNKNOWN;

	// シェーダーバイトコードを取得する (冒頭で取得済み)
	if (!vs || vs->bytecode.empty() || !ps || ps->bytecode.empty())
		return defaultPSOForFormat(rtvFmt);   // フォールバック

	// buildInputLayout: VertexAttribute → セマンティクス変換はグローバルテーブルが担う
	// getOrCreatePSO は後方互換パスのため vertexLayout 情報がない → 空レイアウト
	D3D12_INPUT_ELEMENT_DESC inputElems[GfxVertexLayout::MAX_ELEMENTS] = {};
	GfxVertexLayout emptyLayout;
	uint32_t inputCount = buildInputLayout(emptyLayout, inputElems);

	// PSO 作成
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc               = {};
	psoDesc.pRootSignature                                    = m_defaultRootSig.Get();
	psoDesc.VS                                                = { vs->bytecode.data(), vs->bytecode.size() };

	// RT なし (深度専用パス) のときは PS を設定しない
	if (rtvFmt != DXGI_FORMAT_UNKNOWN)
		psoDesc.PS = { ps->bytecode.data(), ps->bytecode.size() };

	psoDesc.InputLayout = { inputElems, inputCount };

	psoDesc.RasterizerState.FillMode                          = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode                          = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.FrontCounterClockwise             = FALSE;
	psoDesc.RasterizerState.DepthClipEnable                   = TRUE;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;
	psoDesc.SampleMask                                        = UINT_MAX;
	psoDesc.PrimitiveTopologyType                             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	// RT の設定 (MRT 対応)
	if (rtvCount > 0)
	{
		psoDesc.NumRenderTargets = rtvCount;
		for (uint32_t i = 0; i < rtvCount; ++i)
			psoDesc.RTVFormats[i] = rtvFmts[i];
	}
	else
	{
		psoDesc.NumRenderTargets = 0;
	}

	// DS の設定
	if (dsvFmt != DXGI_FORMAT_UNKNOWN)
	{
		psoDesc.DSVFormat                        = dsvFmt;
		psoDesc.DepthStencilState.DepthEnable    = TRUE;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
		psoDesc.DepthStencilState.StencilEnable  = FALSE;
	}
	else
	{
		psoDesc.DSVFormat                        = DXGI_FORMAT_UNKNOWN;
		psoDesc.DepthStencilState.DepthEnable    = FALSE;
		psoDesc.DepthStencilState.StencilEnable  = FALSE;
	}

	psoDesc.SampleDesc = { 1, 0 };

	ComPtr<ID3D12PipelineState> pso;
	if (FAILED(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))))
	{
		OutputDebugStringA("[GfxEngine] getOrCreatePSO: CreateGraphicsPipelineState failed\n");
		return defaultPSOForFormat(rtvFmt);   // フォールバック
	}

	m_psoCache.emplace(key, std::move(pso));

	// PSO を新規作成したときはログを出力する
	// FPS スパイクと重なる場合は PSO キャッシュミスが原因
	char msg[256];
	snprintf(msg, sizeof(msg),
		"[GfxEngine] PSO created: vsId=%llu psId=%llu rtvCount=%u rtvFmt[0]=%u dsvFmt=%u layoutHash=%llu\n",
		(unsigned long long)vsId,
		(unsigned long long)psId,
		rtvCount,
		rtvCount > 0 ? (unsigned)rtvFmts[0] : 0,
		(unsigned)dsvFmt,
		(unsigned long long)key.layoutHash);
	OutputDebugStringA(msg);

	return m_psoCache.at(key).Get();
}
