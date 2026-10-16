#pragma once
// ============================================================
//  gfx/abi/Compat.h
//  v1 → v2 互換レイヤー (GfxPipelineDesc を v2 記述子に変換する)
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "Resources.h"
// ============================================================
//  v1 → v2 互換レイヤー
//  GfxPipelineDesc (v1) を GfxGraphicsPipelineDesc + GfxBindingLayoutDesc に変換する。
//  outPipeline.bindingLayout は呼び出し側が outLayout から作成して設定する。
// ============================================================

// PixelFormat (v1) → GfxFormat
inline GfxFormat gfxToFormat(PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::RGBA8_UNORM:       return GfxFormat::R8G8B8A8_Unorm;
	case PixelFormat::BGRA8_UNORM:       return GfxFormat::B8G8R8A8_Unorm;
	case PixelFormat::RGBA16_FLOAT:      return GfxFormat::R16G16B16A16_Float;
	case PixelFormat::R32_FLOAT:         return GfxFormat::R32_Float;
	case PixelFormat::D32_FLOAT:         return GfxFormat::D32_Float;
	case PixelFormat::D24_UNORM_S8_UINT: return GfxFormat::D24_Unorm_S8_Uint;
	case PixelFormat::D16_UNORM:         return GfxFormat::D16_Unorm;
	default:                             return GfxFormat::Unknown;
	}
}

// GfxVertexLayout (v1) → GfxInputLayoutDesc
//  v1 は R32_FLOAT 指定をセマンティクスごとの既定の幅 (float2/3/4) に展開する。
inline GfxInputLayoutDesc gfxConvertVertexLayout(const GfxVertexLayout& layout)
{
	GfxInputLayoutDesc out;
	if (layout.stride == 0)   // stride = 0 は InputLayout なし
		return out;

	for (uint32_t i = 0; i < layout.elementCount; ++i)
	{
		const GfxVertexElement& e = layout.elements[i];
		const char* sem      = "POSITION";
		uint32_t    semIndex = 0;
		GfxFormat   wide     = GfxFormat::R32G32B32_Float;
		switch (e.attribute)
		{
		case VertexAttribute::Position:  sem = "POSITION";                wide = GfxFormat::R32G32B32_Float;    break;
		case VertexAttribute::Normal:    sem = "NORMAL";                  wide = GfxFormat::R32G32B32_Float;    break;
		case VertexAttribute::Tangent:   sem = "TANGENT";                 wide = GfxFormat::R32G32B32A32_Float; break;
		case VertexAttribute::TexCoord0: sem = "TEXCOORD";                wide = GfxFormat::R32G32_Float;       break;
		case VertexAttribute::TexCoord1: sem = "TEXCOORD"; semIndex = 1;  wide = GfxFormat::R32G32_Float;       break;
		case VertexAttribute::Color0:    sem = "COLOR";                   wide = GfxFormat::R32G32B32_Float;    break;
		}
		const GfxFormat fmt = (e.format == PixelFormat::R32_FLOAT) ? wide : gfxToFormat(e.format);
		out.add(sem, semIndex, fmt, 0, e.offset);
	}
	return out;
}

// GfxRootSignatureDesc (v1) → GfxBindingLayoutDesc
inline GfxBindingLayoutDesc gfxConvertRootSignature(const GfxRootSignatureDesc& rs)
{
	auto toVis = [](ShaderVisibility v)
	{
		switch (v)
		{
		case ShaderVisibility::Vertex: return GfxShaderVisibility::Vertex;
		case ShaderVisibility::Pixel:  return GfxShaderVisibility::Pixel;
		default:                       return GfxShaderVisibility::All;
		}
	};

	GfxBindingLayoutDesc out;
	out.flags = GfxRootSignatureFlags::AllowInputAssemblerInputLayout;
	for (uint32_t i = 0; i < rs.paramCount && i < GfxRootSignatureDesc::MAX_PARAMS; ++i)
	{
		const GfxRootParam& p = rs.params[i];
		switch (p.type)
		{
		case RootParamType::CBV:
			out.addRootDescriptor(GfxRootParameterType::CBV, p.shaderRegister, p.registerSpace, toVis(p.visibility));
			break;
		case RootParamType::SamplerTable:
			out.beginTable(toVis(p.visibility))
			   .addRange(GfxDescriptorRangeType::Sampler, p.descriptorCount, p.shaderRegister, p.registerSpace);
			break;
		case RootParamType::SRVTable:
		case RootParamType::SRVTableVS:
			out.beginTable(p.type == RootParamType::SRVTableVS ? GfxShaderVisibility::Vertex : toVis(p.visibility))
			   .addRange(GfxDescriptorRangeType::SRV, p.descriptorCount, p.shaderRegister, p.registerSpace,
						 GfxDescriptorRangeFlags::DescriptorsVolatile);
			break;
		}
	}
	return out;
}

// GfxPipelineDesc (v1) → GfxGraphicsPipelineDesc
inline GfxGraphicsPipelineDesc gfxConvertPipelineDesc(const GfxPipelineDesc& v1)
{
	GfxGraphicsPipelineDesc out;
	out.vs = v1.vs;
	out.ps = v1.ps;

	GfxRenderTargetBlendDesc& rt0 = out.blendState.renderTarget[0];
	if (v1.blendMode == BlendMode::Alpha)
	{
		rt0.blendEnable = true;
		rt0.srcBlend    = GfxBlend::SrcAlpha;
		rt0.destBlend   = GfxBlend::InvSrcAlpha;
	}
	else if (v1.blendMode == BlendMode::Additive)
	{
		rt0.blendEnable = true;
		rt0.srcBlend    = GfxBlend::One;
		rt0.destBlend   = GfxBlend::One;
	}

	out.rasterizerState.fillMode = (v1.fillMode == FillMode::Wireframe) ? GfxFillMode::Wireframe : GfxFillMode::Solid;
	out.rasterizerState.cullMode = (v1.cullMode == CullMode::Back)  ? GfxCullMode::Back
	                             : (v1.cullMode == CullMode::Front) ? GfxCullMode::Front
	                             :                                    GfxCullMode::None;

	// v1 は DSV がないときは深度ステートを使わない
	out.dsvFormat = gfxToFormat(v1.dsvFormat);
	if (out.dsvFormat != GfxFormat::Unknown)
	{
		out.depthStencilState.depthEnable    = v1.depthEnable;
		out.depthStencilState.depthWriteMask = v1.depthWrite ? GfxDepthWriteMask::All : GfxDepthWriteMask::Zero;
	}
	else
	{
		out.depthStencilState.depthEnable    = false;
		out.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
	}

	out.inputLayout = gfxConvertVertexLayout(v1.vertexLayout);

	if (v1.topology == Topology::LineList)
	{
		out.primitiveTopologyType = GfxPrimitiveTopologyType::Line;
		out.primitiveTopology     = GfxPrimitiveTopology::LineList;
	}

	out.numRenderTargets = v1.rtvCount < GFX_MAX_RENDER_TARGETS ? v1.rtvCount : GFX_MAX_RENDER_TARGETS;
	for (uint32_t i = 0; i < out.numRenderTargets; ++i)
		out.rtvFormats[i] = gfxToFormat(v1.rtvFormats[i]);

	out.sampleCount = v1.sampleCount > 0 ? v1.sampleCount : 1;
	out.rasterizerState.multisampleEnable = out.sampleCount > 1;
	out.debugName = v1.debugName;
	return out;
}
