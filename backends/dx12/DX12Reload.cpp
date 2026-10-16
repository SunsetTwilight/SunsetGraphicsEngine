// ============================================================
//  backends/dx12/DX12Reload.cpp
//  シェーダーとパイプラインの作り直し (ホットリロード / エディターでの記述子の変更)
//
//  どちらも「全部作れたら入れ替える」。途中で失敗したら何も変えずに false を返すので、
//  壊れたシェーダーを保存しても、直前の正しい状態で描画を続けられる。
//  古い PSO は、記録済み / 実行中のコマンドが参照しているので遅延解放する。
//  ※ 記録中のコマンドバッファでバインド済みのパイプラインを、同じコマンドバッファの記録中に
//    更新しないこと (バインド済みのルートシグネチャと、以降のバインドの対応がずれる)。
// ============================================================

#include "DX12Device.h"

DX12PipelineImpl* DX12DeviceImpl::findPipeline(PipelineHandle h) const
{
	auto it = m_pipelines.find(h.id);
	return it != m_pipelines.end() ? it->second : nullptr;
}

bool DX12DeviceImpl::updateShader(ShaderHandle h, const GfxShaderDesc* desc)
{
	auto it = m_shaders.find(h.id);
	if (it == m_shaders.end() || !desc)
		return false;

	if (desc->stage != it->second.stage)
	{
		emitDebugMessage(GfxMessageSeverity::Error, GFX_MESSAGE_ID_SHADER_COMPILE,
			"updateShader: シェーダーのステージは変えられません");
		return false;
	}

	// ---- 新しいバイトコードを作る ----
	DX12ShadRes next;
	next.stage = desc->stage;
	if (desc->source)
	{
		const char* profile = desc->targetProfile ? desc->targetProfile : dx12DefaultShaderProfile(desc->stage);
		DX12ShaderOutput out;
		const bool ok = m_shaderCompiler.compile(*desc, profile, out);
		reportShaderMessages("updateShader", desc->sourceName, out.messages, !ok);
		if (!ok)
			return false;
		next.bytecode     = std::move(out.bytecode);
		next.dependencies = std::move(out.dependencies);
		if (out.reflection)
		{
			const uint8_t* r = static_cast<const uint8_t*>(out.reflection->GetBufferPointer());
			next.reflection.assign(r, r + out.reflection->GetBufferSize());
		}
	}
	else if (desc->bytecode && desc->bytecodeSize > 0)
	{
		const uint8_t* p = static_cast<const uint8_t*>(desc->bytecode);
		next.bytecode.assign(p, p + desc->bytecodeSize);
	}
	else
		return false;

	// ---- 使っているパイプラインを新しいバイトコードで作り直す (全部成功したら入れ替える) ----
	std::swap(it->second, next);   // next = 古いシェーダー
	std::vector<std::pair<DX12PipelineImpl*, ComPtr<ID3D12PipelineState>>> rebuilt;
	for (const auto& [id, impl] : m_pipelines)
	{
		if (!impl->usesShader(h.id))
			continue;
		ComPtr<ID3D12PipelineState> pso = impl->rebuildPSO();
		if (!pso)
		{
			std::swap(it->second, next);   // 元に戻す
			emitDebugMessage(GfxMessageSeverity::Error, GFX_MESSAGE_ID_SHADER_COMPILE,
				(std::string("updateShader: 新しいシェーダーでパイプラインを作れないので、元のシェーダーのままにします (")
				 + (desc->sourceName ? desc->sourceName : "unnamed") + ")").c_str());
			return false;
		}
		rebuilt.emplace_back(impl, std::move(pso));
	}

	for (auto& [impl, pso] : rebuilt)
		deferRelease(impl->swapPSO(std::move(pso)));

	// 旧経路 (bindShader) の PSO キャッシュから、このシェーダーを使うものを捨てる
	for (auto c = m_psoCache.begin(); c != m_psoCache.end();)
	{
		if (c->first.vsId == h.id || c->first.psId == h.id)
		{
			deferRelease(c->second);
			c = m_psoCache.erase(c);
		}
		else
			++c;
	}
	return true;
}

namespace
{
	template<typename T>
	bool writeVersionedOut(const T& src, T* out)
	{
		if (!out || out->structSize < 2 * sizeof(uint32_t))
			return false;
		const uint32_t callerSize = out->structSize;
		std::memcpy(static_cast<void*>(out), &src, std::min<size_t>(callerSize, sizeof(T)));
		out->structSize = callerSize;
		return true;
	}
}

bool DX12DeviceImpl::updateGraphicsPipeline(PipelineHandle h, const GfxGraphicsPipelineDesc* src)
{
	DX12PipelineImpl* impl = findPipeline(h);
	if (!impl || impl->isCompute())
		return false;

	GfxGraphicsPipelineDesc d;
	if (!readVersionedDesc(src, d, "updateGraphicsPipeline"))
		return false;
	auto layoutIt = m_bindingLayouts.find(d.bindingLayout.id);
	if (layoutIt == m_bindingLayouts.end())
	{
		emitDebugMessage(GfxMessageSeverity::Error, GFX_MESSAGE_ID_PIPELINE, "updateGraphicsPipeline: bindingLayout が無効です");
		return false;
	}
	for (auto& e : d.inputLayout.elements)
		e.semanticName[GFX_MAX_SEMANTIC_NAME - 1] = '\0';
	if (!d.debugName)
		d.debugName = impl->graphicsDesc().debugName;   // 名前を渡さなければ今の名前のまま

	ComPtr<ID3D12PipelineState> pso = DX12PipelineImpl::buildGraphicsPSO(this, d, layoutIt->second.rootSig.Get());
	if (!pso)
		return false;
	deferRelease(impl->replace(d, layoutIt->second, std::move(pso)));
	return true;
}

bool DX12DeviceImpl::updateComputePipeline(PipelineHandle h, const GfxComputePipelineDesc* src)
{
	DX12PipelineImpl* impl = findPipeline(h);
	if (!impl || !impl->isCompute())
		return false;

	GfxComputePipelineDesc d;
	if (!readVersionedDesc(src, d, "updateComputePipeline"))
		return false;
	auto layoutIt = m_bindingLayouts.find(d.bindingLayout.id);
	if (layoutIt == m_bindingLayouts.end())
	{
		emitDebugMessage(GfxMessageSeverity::Error, GFX_MESSAGE_ID_PIPELINE, "updateComputePipeline: bindingLayout が無効です");
		return false;
	}
	if (!d.debugName)
		d.debugName = impl->computeDesc().debugName;

	ComPtr<ID3D12PipelineState> pso = DX12PipelineImpl::buildComputePSO(this, d, layoutIt->second.rootSig.Get());
	if (!pso)
		return false;
	deferRelease(impl->replace(d, layoutIt->second, std::move(pso)));
	return true;
}

bool DX12DeviceImpl::getGraphicsPipelineDesc(PipelineHandle h, GfxGraphicsPipelineDesc* out) const
{
	const DX12PipelineImpl* impl = findPipeline(h);
	return impl && !impl->isCompute() && writeVersionedOut(impl->graphicsDesc(), out);
}

bool DX12DeviceImpl::getComputePipelineDesc(PipelineHandle h, GfxComputePipelineDesc* out) const
{
	const DX12PipelineImpl* impl = findPipeline(h);
	return impl && impl->isCompute() && writeVersionedOut(impl->computeDesc(), out);
}
