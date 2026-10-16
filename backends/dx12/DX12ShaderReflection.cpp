// ============================================================
//  backends/dx12/DX12ShaderReflection.cpp
//  ID3D12ShaderReflection → GfxShaderReflection (ABI の POD) への変換と、
//  シェーダーのリフレクション / 依存ファイルの問い合わせ
// ============================================================

#include "DX12Device.h"
#include <memory>

namespace
{
	template<size_t N>
	void copyName(char (&dst)[N], const char* src)
	{
		size_t i = 0;
		for (; src && src[i] && i + 1 < N; ++i)
			dst[i] = src[i];
		dst[i] = '\0';
	}

	class ReflectionBuilder
	{
	public:
		explicit ReflectionBuilder(GfxShaderReflection& out) : m_out(out) {}

		void build(ID3D12ShaderReflection* r)
		{
			D3D12_SHADER_DESC sd = {};
			if (FAILED(r->GetDesc(&sd)))
				return;

			const UINT ver = sd.Version;
			m_out.shaderModel      = D3D12_SHVER_GET_MAJOR(ver) * 10 + D3D12_SHVER_GET_MINOR(ver);
			m_out.instructionCount = sd.InstructionCount;
			UINT x = 0, y = 0, z = 0;
			r->GetThreadGroupSize(&x, &y, &z);
			m_out.threadGroupSize[0] = x;
			m_out.threadGroupSize[1] = y;
			m_out.threadGroupSize[2] = z;

			for (UINT i = 0; i < sd.BoundResources; ++i)
			{
				D3D12_SHADER_INPUT_BIND_DESC bd = {};
				if (FAILED(r->GetResourceBindingDesc(i, &bd)))
					continue;
				if (m_out.bindingCount >= GFX_MAX_SHADER_BINDINGS) { m_out.truncated = true; break; }
				GfxShaderBinding& b = m_out.bindings[m_out.bindingCount++];
				copyName(b.name, bd.Name);
				b.type      = static_cast<GfxShaderInputType>(bd.Type);
				b.dimension = static_cast<GfxShaderResourceDimension>(bd.Dimension);
				b.bindPoint = bd.BindPoint;
				b.bindCount = bd.BindCount;
				b.space     = bd.Space;
			}

			for (UINT i = 0; i < sd.ConstantBuffers; ++i)
			{
				ID3D12ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i);
				D3D12_SHADER_BUFFER_DESC bdesc = {};
				if (!cb || FAILED(cb->GetDesc(&bdesc)))
					continue;
				if (m_out.cbufferCount >= GFX_MAX_SHADER_CBUFFERS) { m_out.truncated = true; break; }

				const uint32_t index = m_out.cbufferCount++;
				GfxShaderCBuffer& c = m_out.cbuffers[index];
				copyName(c.name, bdesc.Name);
				c.size          = bdesc.Size;
				c.firstVariable = m_out.variableCount;

				D3D12_SHADER_INPUT_BIND_DESC bd = {};
				if (bdesc.Name && SUCCEEDED(r->GetResourceBindingDescByName(bdesc.Name, &bd)))
				{
					c.bindPoint = bd.BindPoint;
					c.space     = bd.Space;
				}

				for (UINT v = 0; v < bdesc.Variables; ++v)
				{
					ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
					D3D12_SHADER_VARIABLE_DESC vd = {};
					if (!var || FAILED(var->GetDesc(&vd)))
						continue;
					addVariable(index, vd.Name, var->GetType(), vd.StartOffset, vd.Size, 0);
				}
				c.variableCount = m_out.variableCount - c.firstVariable;

				// バインディングから定数バッファを引けるようにする
				for (uint32_t k = 0; k < m_out.bindingCount; ++k)
				{
					GfxShaderBinding& b = m_out.bindings[k];
					if (b.type == GfxShaderInputType::CBuffer && std::strcmp(b.name, c.name) == 0)
						b.cbufferIndex = index;
				}
			}

			addSignature(r, sd.InputParameters,  m_out.inputs,  m_out.inputCount,  true);
			addSignature(r, sd.OutputParameters, m_out.outputs, m_out.outputCount, false);
		}

	private:
		void addVariable(uint32_t cbIndex, const std::string& name, ID3D12ShaderReflectionType* type,
		                 uint32_t offset, uint32_t size, uint32_t depth)
		{
			if (m_out.variableCount >= GFX_MAX_SHADER_VARIABLES) { m_out.truncated = true; return; }

			D3D12_SHADER_TYPE_DESC td = {};
			if (type)
				type->GetDesc(&td);

			GfxShaderVariable& v = m_out.variables[m_out.variableCount++];
			copyName(v.name, name.c_str());
			if (name.size() + 1 > GFX_MAX_SHADER_NAME)
				m_out.truncated = true;
			v.cbufferIndex  = cbIndex;
			v.offset        = offset;
			v.size          = size;
			v.variableClass = static_cast<GfxShaderVariableClass>(td.Class);
			v.variableType  = static_cast<GfxShaderVariableType>(td.Type);
			v.rows          = td.Rows;
			v.columns       = td.Columns;
			v.elements      = td.Elements;
			v.depth         = depth;

			// 構造体はメンバーを parent.member の名前で展開する (配列の構造体は先頭の要素のレイアウト)
			if (td.Class != D3D_SVC_STRUCT || !type || depth >= 8)
				return;
			for (UINT m = 0; m < td.Members; ++m)
			{
				ID3D12ShaderReflectionType* mt = type->GetMemberTypeByIndex(m);
				D3D12_SHADER_TYPE_DESC mtd = {};
				if (!mt || FAILED(mt->GetDesc(&mtd)))
					continue;

				// メンバーのサイズは次のメンバーの位置 (最後のメンバーは親の終わり) までとみなす
				uint32_t end = size / std::max(1u, td.Elements ? td.Elements : 1u);
				if (m + 1 < td.Members)
				{
					D3D12_SHADER_TYPE_DESC next = {};
					if (ID3D12ShaderReflectionType* nt = type->GetMemberTypeByIndex(m + 1))
						if (SUCCEEDED(nt->GetDesc(&next)))
							end = next.Offset;
				}
				const char* memberName = type->GetMemberTypeName(m);
				addVariable(cbIndex, name + "." + (memberName ? memberName : "?"), mt,
				            offset + mtd.Offset, end > mtd.Offset ? end - mtd.Offset : 0, depth + 1);
			}
		}

		void addSignature(ID3D12ShaderReflection* r, UINT count, GfxShaderSignatureParam* dst, uint32_t& dstCount, bool input)
		{
			for (UINT i = 0; i < count; ++i)
			{
				D3D12_SIGNATURE_PARAMETER_DESC pd = {};
				if (FAILED(input ? r->GetInputParameterDesc(i, &pd) : r->GetOutputParameterDesc(i, &pd)))
					continue;
				if (dstCount >= GFX_MAX_SHADER_SIGNATURE) { m_out.truncated = true; return; }
				GfxShaderSignatureParam& p = dst[dstCount++];
				copyName(p.semanticName, pd.SemanticName);
				p.semanticIndex = pd.SemanticIndex;
				p.registerIndex = pd.Register;
				p.systemValue   = static_cast<GfxSystemValue>(pd.SystemValueType);
				p.componentType = static_cast<GfxRegisterComponentType>(pd.ComponentType);
				p.mask          = pd.Mask;
				p.readWriteMask = pd.ReadWriteMask;
			}
		}

		GfxShaderReflection& m_out;
	};
}

bool DX12DeviceImpl::getShaderReflection(ShaderHandle h, GfxShaderReflection* out)
{
	if (!out || out->structSize < sizeof(uint32_t) * 2)
		return false;
	const DX12ShadRes* sh = getShadRes(h.id);
	if (!sh)
		return false;

	// リフレクションのパートがあればそれを、なければ DXIL コンテナから作る
	const std::vector<uint8_t>& src = sh->reflection.empty() ? sh->bytecode : sh->reflection;
	ComPtr<ID3D12ShaderReflection> refl = m_shaderCompiler.createReflection(src.data(), src.size());
	if (!refl)
		return false;

	auto full = std::make_unique<GfxShaderReflection>();
	ReflectionBuilder(*full).build(refl.Get());

	// 呼び出し側の structSize までを書く (古いヘッダーでビルドされたアプリでも壊さない)
	const uint32_t callerSize = out->structSize;
	const size_t   n          = std::min<size_t>(callerSize, sizeof(GfxShaderReflection));
	std::memcpy(out, full.get(), n);
	out->structSize = callerSize;
	return true;
}

uint32_t DX12DeviceImpl::getShaderDependencies(ShaderHandle h, char* buffer, uint32_t bufferSize)
{
	const DX12ShadRes* sh = getShadRes(h.id);
	if (!sh)
		return 0;

	std::string joined;
	for (const auto& d : sh->dependencies)
	{
		if (!joined.empty())
			joined += '\n';
		joined += d;
	}
	const uint32_t required = static_cast<uint32_t>(joined.size() + 1);
	if (buffer && bufferSize >= required)
		std::memcpy(buffer, joined.c_str(), required);
	return required;
}
