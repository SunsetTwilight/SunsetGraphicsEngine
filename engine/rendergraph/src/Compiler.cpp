// ============================================================
//  engine/rendergraph/src/Compiler.cpp
// ============================================================
#include "gfx/rendergraph/Compiler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace gfx::rg
{
// ============================================================
//  公開の小さな関数
// ============================================================
GfxResourceState requiredState(PassType type, Access access)
{
	switch (access)
	{
	case Access::SRV:
		return type == PassType::Compute ? GfxResourceState::NonPixelShaderResource : GfxResourceState::AllShaderResource;
	case Access::UAV:          return GfxResourceState::UnorderedAccess;
	case Access::RTV:          return GfxResourceState::RenderTarget;
	case Access::DSV:          return GfxResourceState::DepthWrite;
	case Access::DSVRead:      return GfxResourceState::DepthRead;
	case Access::CopySrc:      return GfxResourceState::CopySource;
	case Access::CopyDst:      return GfxResourceState::CopyDest;
	case Access::IndirectArgs: return GfxResourceState::IndirectArgument;
	case Access::ResolveSrc:   return GfxResourceState::ResolveSource;
	case Access::ResolveDst:   return GfxResourceState::ResolveDest;
	}
	return GfxResourceState::Common;
}

bool isDepthFormat(GfxFormat f)
{
	return f == GfxFormat::D32_Float || f == GfxFormat::D24_Unorm_S8_Uint
	    || f == GfxFormat::D16_Unorm || f == GfxFormat::D32_Float_S8X24_Uint;
}

std::vector<Diagnostic> CompiledGraph::errors() const
{
	std::vector<Diagnostic> out;
	for (const auto& d : diagnostics)
		if (d.severity == Diagnostic::Severity::Error)
			out.push_back(d);
	return out;
}

std::vector<Diagnostic> CompiledGraph::warnings() const
{
	std::vector<Diagnostic> out;
	for (const auto& d : diagnostics)
		if (d.severity == Diagnostic::Severity::Warning)
			out.push_back(d);
	return out;
}

const ResourceInfo* CompiledGraph::findResource(const std::string& name) const
{
	for (const auto& r : resources)
		if (r.name == name)
			return &r;
	return nullptr;
}

int32_t CompiledGraph::indexOfPass(const std::string& name) const
{
	for (size_t i = 0; i < passes.size(); ++i)
		if (passes[i].name == name)
			return static_cast<int32_t>(i);
	return -1;
}

std::vector<std::string> CompiledGraph::skippedNames(SkippedPass::Reason reason) const
{
	std::vector<std::string> out;
	for (const auto& s : skipped)
		if (s.reason == reason)
			out.push_back(s.name);
	return out;
}

// ============================================================
//  コンパイル
// ============================================================
namespace
{
bool isInputAccess(Access a)
{
	return a == Access::SRV || a == Access::UAV || a == Access::DSVRead || a == Access::CopySrc
	    || a == Access::IndirectArgs || a == Access::ResolveSrc;
}
bool isOutputAccess(Access a)
{
	return a == Access::RTV || a == Access::DSV || a == Access::UAV || a == Access::CopyDst || a == Access::ResolveDst;
}
bool isCopyAccess(Access a)
{
	return a == Access::CopySrc || a == Access::CopyDst || a == Access::ResolveSrc || a == Access::ResolveDst;
}
// シェーダーから参照するので bind が要る使い方
bool needsBind(PassType type, Access a)
{
	return type != PassType::Copy && (a == Access::SRV || a == Access::UAV);
}

std::string refName(const PassResourceRef& r)
{
	return r.as == Access::RTV ? "RTV" + std::to_string(r.rtvIndex) : nameOf(r.as);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
	if (from.empty()) return s;
	for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
		s.replace(pos, from.size(), to);
	return s;
}

// 1 つのパスでの、1 つのサブリソースの使い方をまとめたもの
struct SubUse
{
	bool             read  = false;
	bool             write = false;
	bool             uav   = false;
	GfxResourceState state = GfxResourceState::Common;
	std::string      first;   // 最初の使い方の名前 (診断用)
	std::string      path;    // 最初に参照した場所
};

// 展開したパス
struct ExpPass
{
	uint32_t    descIndex = 0;
	std::string name;
	int32_t     iteration = -1;
	PassDesc    d;          // 変数を値に置き換えたもの
	std::string path;       // "passes[Name[k]]"
};

// ディスクリプタテーブルの offset の位置にあるディスクリプタの種類。範囲外なら false
bool tableTypeAt(const GfxBindingLayoutDesc& layout, const GfxRootParameter& p, uint32_t offset,
                 GfxDescriptorRangeType& out, uint64_t& tableSize)
{
	uint64_t cursor = 0;
	tableSize = 0;
	bool found = false;
	for (uint32_t i = 0; i < p.numRanges; ++i)
	{
		const uint32_t ri = p.firstRange + i;
		if (ri >= layout.numRanges || ri >= GFX_MAX_DESCRIPTOR_RANGES)
			break;
		const GfxDescriptorRange& r = layout.ranges[ri];
		const uint64_t start = r.offsetInDescriptorsFromTableStart == GFX_DESCRIPTOR_RANGE_OFFSET_APPEND
			? cursor : r.offsetInDescriptorsFromTableStart;
		const uint64_t count = r.numDescriptors == GFX_UNBOUNDED_DESCRIPTOR_COUNT ? 0xFFFFFFFFull : r.numDescriptors;
		if (!found && offset >= start && offset < start + count)
		{
			out   = r.rangeType;
			found = true;
		}
		cursor    = start + count;
		tableSize = std::max(tableSize, cursor);
	}
	return found;
}

const char* rangeTypeName(GfxDescriptorRangeType t)
{
	switch (t)
	{
	case GfxDescriptorRangeType::SRV:     return "SRV";
	case GfxDescriptorRangeType::UAV:     return "UAV";
	case GfxDescriptorRangeType::CBV:     return "CBV";
	case GfxDescriptorRangeType::Sampler: return "Sampler";
	}
	return "?";
}

class Compiler
{
public:
	Compiler(const GraphDesc& g, const CompileOptions& o) : m_g(g), m_opt(o) {}

	CompiledGraph run()
	{
		resolveParameters();
		collectResources();
		expandPasses();
		for (uint32_t e = 0; e < m_passes.size(); ++e)
			analyzePass(e);
		buildDependencies();
		cull();
		if (!sortPasses())
			return finish();
		simulate();
		return finish();
	}

private:
	void error(const std::string& path, const std::string& msg)
	{
		m_out.diagnostics.push_back({ Diagnostic::Severity::Error, path, msg });
	}
	void warning(const std::string& path, const std::string& msg)
	{
		m_out.diagnostics.push_back({ Diagnostic::Severity::Warning, path, msg });
	}

	// ---- 0. パラメーター ----
	void resolveParameters()
	{
		std::set<std::string> names;
		for (const auto& p : m_g.parameters)
		{
			if (p.name.empty() || !names.insert(p.name).second)
			{
				error("parameters." + p.name, "パラメーターの名前が空か、重なっています");
				continue;
			}
			m_params[p.name] = p;
		}
		for (const auto& [name, value] : m_opt.parameters)
		{
			auto it = m_params.find(name);
			if (it == m_params.end())
			{
				warning("parameters." + name, "グラフにないパラメーターなので、上書きを無視します");
				continue;
			}
			it->second.value = value;
		}
		for (const auto& [name, p] : m_params)
			m_out.parameters[name] = p.value;
	}

	// ---- 1. リソース ----
	void collectResources()
	{
		for (size_t i = 0; i < m_g.resources.size(); ++i)
		{
			const ResourceDesc& r = m_g.resources[i];
			const std::string path = "resources." + r.name;
			if (r.name.empty()) { error("resources", "名前のないリソースがあります"); continue; }
			if (m_resIndex.count(r.name)) { error(path, "同じ名前のリソースがすでにあります"); continue; }
			if (r.name.find('@') != std::string::npos)
				error(path, "名前に @ は使えません (name@prev は前フレームの内容を表します)");
			if (isImportedName(r.name) && r.kind != ResourceKind::Imported)
				error(path, "$ で始まる名前はインポート (アプリが渡すリソース) です。kind を imported にするか、名前を変えてください");

			if (r.kind == ResourceKind::Texture)
			{
				if (r.format == GfxFormat::Unknown)
					error(path + ".format", "テクスチャのフォーマットがありません");
				if (r.size.relative && !(r.size.scale > 0.0f))
					error(path + ".size.relative", "比率は 0 より大きい値です");
				if (!r.size.relative && (r.size.width == 0 || r.size.height == 0))
					error(path + ".size", "幅と高さは 1 以上です");
				const uint32_t s = r.sampleCount;
				if (s != 1 && s != 2 && s != 4 && s != 8 && s != 16 && s != 32)
					error(path + ".samples", "サンプル数は 1 / 2 / 4 / 8 / 16 / 32 のどれかです");
				if (s > 1 && r.mipLevels > 1)
					error(path + ".mips", "MSAA のテクスチャは mip を持てません");
				if (s > 1 && r.dimension == TextureDimension::Tex3D)
					error(path + ".samples", "3D のテクスチャは MSAA にできません");
				if (r.dimension == TextureDimension::Tex3D && r.arraySize != 1)
					error(path + ".array", "3D のテクスチャは配列にできません");
				if (r.dimension != TextureDimension::Tex3D && r.depth != 1)
					error(path + ".depth", "depth は 3D のテクスチャだけで使います");
				if (r.dimension == TextureDimension::Cube && !r.size.relative && r.size.width != r.size.height)
					error(path + ".size", "キューブのテクスチャは幅と高さを同じにしてください");
				if (r.clear.hasDepth && !isDepthFormat(r.format))
					warning(path + ".clear.depth", "深度のフォーマットではないので、深度のクリア値は使いません");
			}
			if (r.kind == ResourceKind::Buffer && r.byteSize == 0)
				error(path + ".bytes", "バッファの大きさがありません");
			if (r.history)
			{
				if (r.kind != ResourceKind::Texture)
					error(path + ".history", "前フレームの内容を残せるのはテクスチャだけです");
				else if (r.mipLevels != 1 || r.layers() != 1)
					error(path + ".history", "前フレームの内容を残すテクスチャは、当面は mip / 配列を持てません");
			}

			ResourceInfo info;
			info.name      = r.name;
			info.kind      = r.kind;
			info.descIndex = static_cast<int32_t>(i);
			info.mips      = std::max(1u, r.mips());
			info.layers    = std::max(1u, r.layers());
			m_resIndex[r.name] = static_cast<uint32_t>(m_out.resources.size());
			m_out.resources.push_back(info);
		}
	}

	// 参照されたリソースの位置 (宣言していないインポートと name@prev はここで加える)。見つからなければ -1
	int32_t resolve(const std::string& name, const std::string& path)
	{
		auto it = m_resIndex.find(name);
		if (it != m_resIndex.end())
			return static_cast<int32_t>(it->second);
		std::string base;
		if (isPrevName(name, &base))
		{
			auto bi = m_resIndex.find(base);
			const ResourceDesc* rd = bi != m_resIndex.end() ? descOf(bi->second) : nullptr;
			if (!rd || !rd->history)
			{
				error(path, "'" + base + "' は history: true のリソースではないので、前フレームの内容 (@prev) は読めません");
				return -1;
			}
			ResourceInfo info;
			info.name      = name;
			info.kind      = rd->kind;
			info.descIndex = m_out.resources[bi->second].descIndex;
			info.historyOf = static_cast<int32_t>(bi->second);
			m_resIndex[name] = static_cast<uint32_t>(m_out.resources.size());
			m_out.resources.push_back(info);
			return static_cast<int32_t>(m_out.resources.size() - 1);
		}
		if (isImportedName(name))
		{
			ResourceInfo info;
			info.name = name;
			info.kind = ResourceKind::Imported;
			m_resIndex[name] = static_cast<uint32_t>(m_out.resources.size());
			m_out.resources.push_back(info);
			return static_cast<int32_t>(m_out.resources.size() - 1);
		}
		error(path, "リソース '" + name + "' は宣言されていません (アプリが渡すリソースなら $ で始めてください)");
		return -1;
	}

	const ResourceDesc* descOf(uint32_t resource) const
	{
		const int32_t d = m_out.resources[resource].descIndex;
		return d >= 0 ? &m_g.resources[static_cast<size_t>(d)] : nullptr;
	}

	// ---- 2. パスの展開 (when / repeat) ----
	bool evalCondition(const std::string& when, const std::string& path, bool& out)
	{
		const bool neg  = !when.empty() && when[0] == '!';
		const std::string name = neg ? when.substr(1) : when;
		auto it = m_params.find(name);
		if (it == m_params.end())
		{
			error(path, "パラメーター '" + name + "' はありません");
			return false;
		}
		out = (it->second.value != 0) != neg;
		return true;
	}

	bool resolveIndex(IndexValue& v, const ExpPass& e, const PassDesc& src, const std::string& path)
	{
		if (!v.usesVar())
			return true;
		if (!src.repeat.set)
		{
			error(path, "変数 '" + v.var + "' は repeat のあるパスでだけ使えます");
			return false;
		}
		if (v.var != src.repeat.var)
		{
			error(path, "変数 '" + v.var + "' はありません (repeat の変数は '" + src.repeat.var + "')");
			return false;
		}
		const int32_t value = e.iteration + v.add;
		if (value < 0)
		{
			error(path, formatIndexExpr(v) + " が " + std::to_string(e.iteration) + " のとき負になります");
			return false;
		}
		v = IndexValue::fixed(value);
		return true;
	}

	void expandPasses()
	{
		std::set<std::string> names;
		for (size_t i = 0; i < m_g.passes.size(); ++i)
		{
			const PassDesc& p = m_g.passes[i];
			const uint32_t  pi = static_cast<uint32_t>(i);
			if (p.name.empty()) { error("passes[" + std::to_string(i) + "]", "名前のないパスがあります"); continue; }
			const std::string basePath = "passes[" + p.name + "]";
			if (!names.insert(p.name).second) { error(basePath, "同じ名前のパスがすでにあります"); continue; }
			if (p.name.find('[') != std::string::npos)
				error(basePath, "パスの名前に [ は使えません (繰り返したパスの名前に使います)");
			m_baseNames.insert(p.name);

			if (!p.enabled)
			{
				m_out.skipped.push_back({ p.name, pi, SkippedPass::Reason::Disabled });
				continue;
			}
			if (!p.when.empty())
			{
				bool on = false;
				if (!evalCondition(p.when, basePath + ".when", on))
					continue;
				if (!on)
				{
					m_out.skipped.push_back({ p.name, pi, SkippedPass::Reason::Condition });
					continue;
				}
			}

			uint32_t count = 1;
			if (p.repeat.set)
			{
				count = p.repeat.count;
				if (!p.repeat.countParam.empty())
				{
					auto it = m_params.find(p.repeat.countParam);
					if (it == m_params.end()) { error(basePath + ".repeat.count", "パラメーター '" + p.repeat.countParam + "' はありません"); continue; }
					if (it->second.isBool)    { error(basePath + ".repeat.count", "パラメーター '" + p.repeat.countParam + "' は整数ではありません"); continue; }
					if (it->second.value < 0 || it->second.value > 4096)
					{
						error(basePath + ".repeat.count", "繰り返しの数は 0〜4096 です");
						continue;
					}
					count = static_cast<uint32_t>(it->second.value);
				}
				if (p.repeat.var.empty())
					error(basePath + ".repeat.var", "変数の名前が空です");
				if (count == 0)
				{
					m_out.skipped.push_back({ p.name, pi, SkippedPass::Reason::Condition });
					continue;
				}
			}

			for (uint32_t k = 0; k < count; ++k)
			{
				ExpPass e;
				e.descIndex = pi;
				e.iteration = p.repeat.set ? p.repeat.start + static_cast<int32_t>(k) : -1;
				e.name      = p.repeat.set ? p.name + "[" + std::to_string(k) + "]" : p.name;
				e.path      = "passes[" + e.name + "]";
				e.d         = p;
				e.d.name    = e.name;
				e.d.repeat  = {};
				e.d.when.clear();
				if (p.repeat.set)
					e.d.view = replaceAll(p.view, "{" + p.repeat.var + "}", std::to_string(e.iteration));
				bool good = true;
				for (const char* key : { "inputs", "outputs" })
				{
					auto& refs = std::string(key) == "inputs" ? e.d.inputs : e.d.outputs;
					for (size_t r = 0; r < refs.size(); ++r)
					{
						const std::string rp = e.path + "." + key + "[" + std::to_string(r) + "]";
						good &= resolveIndex(refs[r].mip, e, p, rp + ".mip");
						good &= resolveIndex(refs[r].slice, e, p, rp + ".slice");
					}
				}
				if (!good)
					break;   // 同じ間違いを繰り返しの数だけ出さない
				m_byBase[p.name].push_back(static_cast<uint32_t>(m_passes.size()));
				m_passByName[e.name] = static_cast<uint32_t>(m_passes.size());
				m_passes.push_back(std::move(e));
			}
		}
		m_uses.resize(m_passes.size());
	}

	// ---- 3. 検証と、パスごとの使い方 ----
	struct Range { uint32_t m0 = 0, mc = 1, s0 = 0, sc = 1; };

	// 参照のサブリソースの範囲。間違っていれば false
	bool rangeOf(const PassResourceRef& r, const ResourceInfo& info, const ResourceDesc* rd, const std::string& path, Range& out)
	{
		const bool sub = !r.mip.all() || !r.slice.all();
		if (!rd || rd->kind != ResourceKind::Texture || info.isPrev())
		{
			if (sub)
			{
				const char* why = !rd || info.imported() ? "インポートしたリソース" : info.isPrev() ? "前フレームの内容" : "バッファ";
				error(path, std::string(why) + "では mip / slice を指定できません");
				return false;
			}
			out = {};
			return true;
		}
		if (rd->dimension == TextureDimension::Tex3D && !r.slice.all())
		{
			error(path + ".slice", "3D のテクスチャはスライスを指定できません");
			return false;
		}
		out.m0 = r.mip.all() ? 0 : static_cast<uint32_t>(r.mip.value);
		out.mc = r.mip.all() ? info.mips : r.mipCount;
		out.s0 = r.slice.all() ? 0 : static_cast<uint32_t>(r.slice.value);
		out.sc = r.slice.all() ? info.layers : r.sliceCount;
		if (out.m0 + out.mc > info.mips)
		{
			error(path + ".mip", "mip " + std::to_string(out.m0) + (out.mc > 1 ? "〜" + std::to_string(out.m0 + out.mc - 1) : "")
			                   + " はありません ('" + r.res + "' の mip は " + std::to_string(info.mips) + " 段)");
			return false;
		}
		if (out.s0 + out.sc > info.layers)
		{
			error(path + ".slice", "スライス " + std::to_string(out.s0) + (out.sc > 1 ? "〜" + std::to_string(out.s0 + out.sc - 1) : "")
			                     + " はありません ('" + r.res + "' のスライスは " + std::to_string(info.layers) + " 枚)");
			return false;
		}
		if ((r.as == Access::RTV || r.as == Access::DSV || r.as == Access::DSVRead) && out.mc != 1)
		{
			error(path + ".mip", refName(r) + " には mip を 1 つ指定してください ('" + r.res + "' は mip を " + std::to_string(info.mips) + " 段持ちます)");
			return false;
		}
		return true;
	}

	void checkRegion(const Region& r, const std::string& path)
	{
		if (!r.set) return;
		if (!(r.width > 0) || !(r.height > 0))
			error(path, "幅と高さは 0 より大きい値です");
		if (r.x < 0 || r.y < 0)
			error(path, "x / y は 0 以上です");
		if (r.relative && (r.x + r.width > 1.0001f || r.y + r.height > 1.0001f))
			error(path, "比率で指定するときは、出力の範囲 (0〜1) に収めてください");
	}

	void analyzePass(uint32_t ei)
	{
		const ExpPass&     e    = m_passes[ei];
		const PassDesc&    p    = e.d;
		const std::string& path = e.path;
		auto& uses = m_uses[ei];   // リソース → (サブリソース → 使い方)

		// ---- パイプラインと内容 ----
		if (p.type != PassType::Copy && p.pipeline.empty())
			error(path + ".pipeline", "パイプラインのアセットがありません");
		if (p.type == PassType::Copy && !p.pipeline.empty())
			warning(path + ".pipeline", "Copy のパスはパイプラインを使わないので無視します");
		const ContentKind ck = p.content.kind;
		if (p.type == PassType::Raster && (ck == ContentKind::Dispatch || ck == ContentKind::DispatchIndirect))
			error(path + ".content", "Raster のパスでは dispatch できません");
		if (p.type == PassType::Compute && (ck == ContentKind::DrawList || ck == ContentKind::Fullscreen || ck == ContentKind::DrawIndirect))
			error(path + ".content", "Compute のパスでは描画できません (dispatch / dispatchIndirect / custom)");
		if (p.type != PassType::Copy && ck == ContentKind::None)
			warning(path + ".content", "何も描きません (content がありません)");
		if (p.type == PassType::Copy && ck != ContentKind::None && ck != ContentKind::Custom)
			warning(path + ".content", "Copy のパスでは content を使いません");
		if (ck == ContentKind::DrawIndirect || ck == ContentKind::DispatchIndirect)
		{
			auto hasIndirectInput = [&](const std::string& res) {
				return std::any_of(p.inputs.begin(), p.inputs.end(), [&](const PassResourceRef& r) { return r.res == res && r.as == Access::IndirectArgs; });
			};
			const std::string ip = path + ".content." + nameOf(ck);
			if (p.content.indirect.args.empty())
				error(ip + ".args", "引数のバッファがありません");
			else if (!hasIndirectInput(p.content.indirect.args))
				error(ip + ".args", "'" + p.content.indirect.args + "' を入力 (inputs) に IndirectArgs で書いてください");
			if (!p.content.indirect.count.empty() && !hasIndirectInput(p.content.indirect.count))
				error(ip + ".count", "'" + p.content.indirect.count + "' を入力 (inputs) に IndirectArgs で書いてください");
		}
		if (p.type != PassType::Raster && (p.viewport.set || p.scissor.set))
			warning(path + ".viewport", "ビューポート / シザーは Raster のパスだけで使います");
		checkRegion(p.viewport, path + ".viewport");
		checkRegion(p.scissor, path + ".scissor");

		// ---- 入力と出力 ----
		std::set<uint32_t> rtvIndices;
		bool hasDSV = false, hasTarget = false;

		auto use = [&](const PassResourceRef& r, const std::string& refPath, bool output) {
			if (r.res.empty()) { error(refPath + ".res", "リソースの名前がありません"); return; }
			const int32_t ri = resolve(r.res, refPath + ".res");
			if (ri < 0) return;
			const ResourceInfo info = m_out.resources[static_cast<size_t>(ri)];
			const ResourceDesc* rd  = descOf(static_cast<uint32_t>(ri));
			const bool isBuffer  = rd && rd->kind == ResourceKind::Buffer;
			const bool isTexture = rd && rd->kind == ResourceKind::Texture;

			// 入力 / 出力に置ける使い方か
			if (!output && !isInputAccess(r.as))
			{
				error(refPath + ".as", std::string(nameOf(r.as)) + " は出力 (outputs) に書いてください");
				return;
			}
			if (output && !isOutputAccess(r.as))
			{
				error(refPath + ".as", std::string(nameOf(r.as)) + " は入力 (inputs) に書いてください");
				return;
			}
			if (info.isPrev() && (output || (r.as != Access::SRV && r.as != Access::CopySrc)))
			{
				error(refPath + ".res", "前フレームの内容 (@prev) は SRV / CopySrc で読むことだけができます");
				return;
			}
			// パスの種類と使い方
			if (p.type == PassType::Copy && !isCopyAccess(r.as))
			{
				error(refPath + ".as", "Copy のパスでは CopySrc / CopyDst / ResolveSrc / ResolveDst だけを使えます");
				return;
			}
			if (p.type != PassType::Copy && isCopyAccess(r.as))
			{
				error(refPath + ".as", std::string(nameOf(r.as)) + " は Copy のパスで使います");
				return;
			}
			if (p.type == PassType::Compute && (r.as == Access::RTV || r.as == Access::DSV || r.as == Access::DSVRead))
			{
				error(refPath + ".as", "Compute のパスでは RTV / DSV を使えません");
				return;
			}
			// リソースの種類とフォーマット
			if (isBuffer && (r.as == Access::RTV || r.as == Access::DSV || r.as == Access::DSVRead || r.as == Access::ResolveSrc || r.as == Access::ResolveDst))
				error(refPath + ".as", "バッファは " + refName(r) + " に使えません");
			if (isTexture && r.as == Access::IndirectArgs)
				error(refPath + ".as", "間接描画の引数にはバッファが必要です");
			if (isTexture && r.as == Access::RTV && isDepthFormat(rd->format))
				error(refPath + ".as", "深度のフォーマットのテクスチャは RTV に使えません (DSV を使ってください)");
			if (isTexture && (r.as == Access::DSV || r.as == Access::DSVRead) && !isDepthFormat(rd->format))
				error(refPath + ".as", "DSV には深度のフォーマットのテクスチャが必要です");
			if (isTexture && (r.as == Access::RTV || r.as == Access::DSV) && rd->dimension == TextureDimension::Tex3D)
				error(refPath + ".as", "3D のテクスチャには、当面は RTV / DSV で描けません (UAV を使ってください)");
			if (isTexture && r.as == Access::ResolveSrc && rd->sampleCount < 2)
				error(refPath + ".as", "ResolveSrc には MSAA (samples > 1) のテクスチャが必要です");
			if (isTexture && r.as == Access::ResolveDst && rd->sampleCount != 1)
				error(refPath + ".as", "ResolveDst には MSAA でないテクスチャが必要です");
			if (r.as == Access::RTV)
			{
				hasTarget = true;
				if (r.rtvIndex >= GFX_MAX_RENDER_TARGETS)
					error(refPath + ".as", "RTV の番号は 0〜7 です");
				else if (!rtvIndices.insert(r.rtvIndex).second)
					error(refPath + ".as", refName(r) + " が重なっています");
			}
			if (r.as == Access::DSV)
			{
				hasTarget = true;
				if (hasDSV) error(refPath + ".as", "DSV は 1 つのパスに 1 つだけです");
				hasDSV = true;
			}
			// load
			if (output && r.load == LoadOp::Clear && r.as != Access::RTV && r.as != Access::DSV)
				error(refPath + ".load", "clear は RTV / DSV だけで使えます");

			// bind
			if (needsBind(p.type, r.as))
			{
				if (!r.bind.valid())
					error(refPath + ".bind", "bind がありません (シェーダーのどこにつなぐかを { \"param\": 番号, \"offset\": 位置 } で指定してください)");
			}
			else if (r.bind.valid())
				warning(refPath + ".bind", refName(r) + " は bind を使わないので無視します");

			// サブリソースの範囲
			Range rg;
			if (!rangeOf(r, info, rd, refPath, rg))
				return;

			// 読み書きの記録 (サブリソースごと)
			const bool write = output;
			const bool read  = !output || (r.load == LoadOp::Load && r.as != Access::CopyDst && r.as != Access::ResolveDst);
			const bool uav   = r.as == Access::UAV;
			const GfxResourceState st = requiredState(p.type, r.as);
			auto& subs = uses[static_cast<uint32_t>(ri)];
			for (uint32_t s = rg.s0; s < rg.s0 + rg.sc; ++s)
				for (uint32_t m = rg.m0; m < rg.m0 + rg.mc; ++m)
				{
					const uint32_t sub = m + s * info.mips;
					SubUse& u = subs[sub];
					if (!u.first.empty())
					{
						// 同じサブリソースを同じパスで 2 回使う: UAV を入力と出力に書いた場合だけ許す
						const bool bothUav = u.uav && uav && (u.write != write);
						if (!bothUav && (write || u.write || u.uav != uav))
						{
							error(refPath, "リソース '" + r.res + "' の同じサブリソース (mip " + std::to_string(m) + ", スライス " + std::to_string(s)
							             + ") を同じパスで " + u.first + " と " + refName(r) + " に使っています (読みながら書くことはできません)");
							return;
						}
						u.state = write ? st : (u.write ? u.state : (u.state | st));
					}
					else
					{
						u.first = refName(r);
						u.path  = refPath;
						u.state = st;
					}
					u.uav   = u.uav || uav;
					u.read  = u.read || read;
					u.write = u.write || write;
				}
			if (write && info.imported())
				m_roots.insert(ei);
		};

		for (size_t i = 0; i < p.inputs.size(); ++i)
			use(p.inputs[i], path + ".inputs[" + std::to_string(i) + "]", false);
		for (size_t i = 0; i < p.outputs.size(); ++i)
			use(p.outputs[i], path + ".outputs[" + std::to_string(i) + "]", true);

		if (p.type == PassType::Raster && !hasTarget)
			error(path + ".outputs", "Raster のパスには RTV か DSV の出力が必要です");
		if (p.keep)
			m_roots.insert(ei);

		// ---- 定数 ----
		for (size_t i = 0; i < p.constants.size(); ++i)
		{
			const ConstantRef& c  = p.constants[i];
			const std::string  cp = path + ".constants[" + std::to_string(i) + "]";
			if (p.type == PassType::Copy) { warning(cp, "Copy のパスでは定数を使わないので無視します"); continue; }
			if (c.source.empty()) error(cp + ".source", "定数の出どころがありません (\"frame\" などの名前か \"inline\")");
			if (!c.bind.valid())  error(cp + ".bind", "bind がありません");
			if (c.source == "inline" && c.values.empty()) error(cp + ".values", "inline の定数には values が必要です");
			if (c.source != "inline" && !c.values.empty()) warning(cp + ".values", "inline ではないので values は使いません");
		}

		validateBindings(ei);
	}

	// ---- bind: 重なりと、バインディングレイアウトとの照合 ----
	void validateBindings(uint32_t ei)
	{
		const PassDesc&    p    = m_passes[ei].d;
		const std::string& path = m_passes[ei].path;
		if (p.type == PassType::Copy)
			return;

		struct Item { Binding bind; std::string path; GfxDescriptorRangeType kind; bool constant; const ConstantRef* c; bool texture; std::string res; };
		std::vector<Item> items;
		auto addRef = [&](const std::vector<PassResourceRef>& refs, const char* key) {
			for (size_t i = 0; i < refs.size(); ++i)
			{
				const PassResourceRef& r = refs[i];
				if (!needsBind(p.type, r.as) || !r.bind.valid())
					continue;
				const auto it = m_resIndex.find(r.res);
				const ResourceDesc* rd = it != m_resIndex.end() ? descOf(it->second) : nullptr;
				items.push_back({ r.bind, path + "." + key + "[" + std::to_string(i) + "].bind",
				                  r.as == Access::UAV ? GfxDescriptorRangeType::UAV : GfxDescriptorRangeType::SRV,
				                  false, nullptr, !rd || rd->kind != ResourceKind::Buffer, r.res });
			}
		};
		addRef(p.inputs, "inputs");
		addRef(p.outputs, "outputs");
		for (size_t i = 0; i < p.constants.size(); ++i)
			if (p.constants[i].bind.valid())
				items.push_back({ p.constants[i].bind, path + ".constants[" + std::to_string(i) + "].bind",
				                  GfxDescriptorRangeType::CBV, true, &p.constants[i], false, {} });

		// 同じ場所に 2 つつないでいないか (同じ UAV を入力と出力に書いて読み書きする場合は、同じ場所でよい)
		std::map<std::pair<int32_t, uint32_t>, const Item*> seen;
		for (const auto& it : items)
		{
			const auto key = std::make_pair(it.bind.param, it.bind.offset);
			auto [pos, inserted] = seen.emplace(key, &it);
			if (inserted)
				continue;
			const Item& first = *pos->second;
			const bool sameUav = !it.constant && !first.constant && it.kind == GfxDescriptorRangeType::UAV
			                  && first.kind == GfxDescriptorRangeType::UAV && it.res == first.res;
			if (!sameUav)
				error(it.path, "bind (param " + std::to_string(it.bind.param) + ", offset " + std::to_string(it.bind.offset)
				             + ") は " + first.path + " と重なっています");
		}

		// バインディングレイアウトとの照合
		if (!m_opt.bindingLayout || p.pipeline.empty() || items.empty())
			return;
		const GfxBindingLayoutDesc* layout = m_opt.bindingLayout(p.pipeline);
		if (!layout)
		{
			warning(path + ".pipeline", "バインディングレイアウトが分からないので、bind を確かめていません");
			return;
		}
		for (const auto& it : items)
		{
			const uint32_t param = static_cast<uint32_t>(it.bind.param);
			if (param >= layout->numParameters || param >= GFX_MAX_ROOT_PARAMETERS)
			{
				error(it.path, "param " + std::to_string(param) + " はありません (パラメーターは " + std::to_string(layout->numParameters) + " 個)");
				continue;
			}
			const GfxRootParameter& rp = layout->parameters[param];
			const char* want = rangeTypeName(it.kind);
			switch (rp.parameterType)
			{
			case GfxRootParameterType::DescriptorTable:
			{
				GfxDescriptorRangeType t = GfxDescriptorRangeType::SRV;
				uint64_t size = 0;
				if (!tableTypeAt(*layout, rp, it.bind.offset, t, size))
					error(it.path, "offset " + std::to_string(it.bind.offset) + " は param " + std::to_string(param)
					             + " のテーブルの範囲外です (大きさ " + std::to_string(size) + ")");
				else if (t != it.kind)
					error(it.path, "param " + std::to_string(param) + " のテーブルの offset " + std::to_string(it.bind.offset)
					             + " は " + rangeTypeName(t) + " です (" + want + " が必要です)");
				break;
			}
			case GfxRootParameterType::Constants32Bit:
				if (!it.constant)
					error(it.path, "param " + std::to_string(param) + " はルート定数です (" + want + " はつなげません)");
				else if (it.c && it.c->source != "inline")
					error(it.path, "ルート定数には inline の値だけを置けます (定数バッファはルート CBV かテーブルにつないでください)");
				else if (it.c && it.bind.offset + it.c->values.size() > rp.num32BitValues)
					error(it.path, "値が " + std::to_string(it.c->values.size()) + " 個で、ルート定数 (" + std::to_string(rp.num32BitValues)
					             + " 個) の offset " + std::to_string(it.bind.offset) + " からは入りません");
				break;
			case GfxRootParameterType::CBV:
			case GfxRootParameterType::SRV:
			case GfxRootParameterType::UAV:
			{
				const GfxDescriptorRangeType t = rp.parameterType == GfxRootParameterType::CBV ? GfxDescriptorRangeType::CBV
				                               : rp.parameterType == GfxRootParameterType::SRV ? GfxDescriptorRangeType::SRV
				                               :                                                  GfxDescriptorRangeType::UAV;
				if (t != it.kind)
					error(it.path, "param " + std::to_string(param) + " はルートの " + rangeTypeName(t) + " です (" + want + " が必要です)");
				else if (!it.constant && it.texture)
					error(it.path, "ルートの SRV / UAV にはバッファだけをつなげます (テクスチャはテーブルにつないでください)");
				if (it.bind.offset != 0)
					warning(it.path, "ルートのディスクリプタでは offset を使いません");
				break;
			}
			}
		}
	}

	// ---- 4. 依存 ----
	void addEdge(uint32_t from, uint32_t to, bool raw)
	{
		if (from == to) return;
		m_succ[from].insert(to);
		m_pred[to].insert(from);
		if (raw) m_rawPred[to].insert(from);
	}

	void buildDependencies()
	{
		// サブリソースごとに、宣言順でたどる
		std::set<std::pair<uint32_t, uint32_t>> reported;   // (パス, リソース): 同じ間違いを 1 回だけ出す
		for (uint32_t ri = 0; ri < m_out.resources.size(); ++ri)
		{
			const ResourceInfo& info = m_out.resources[ri];
			if (info.isPrev())
				continue;   // 前フレームの内容は、このフレームのパスに依存しない
			std::map<uint32_t, int32_t> lastWriter;
			std::map<uint32_t, std::vector<uint32_t>> readers;
			for (uint32_t ei = 0; ei < m_passes.size(); ++ei)
			{
				auto it = m_uses[ei].find(ri);
				if (it == m_uses[ei].end())
					continue;
				for (const auto& [sub, u] : it->second)
				{
					auto lw = lastWriter.find(sub);
					const int32_t writer = lw != lastWriter.end() ? lw->second : -1;
					if (u.read)
					{
						if (writer >= 0)
							addEdge(static_cast<uint32_t>(writer), ei, true);
						else if (!info.imported() && reported.insert({ ei, ri }).second)
						{
							const std::string where = info.mips * info.layers > 1
								? " (mip " + std::to_string(sub % info.mips) + ", スライス " + std::to_string(sub / info.mips) + ")" : "";
							if (u.write)
								warning(u.path + ".load", "'" + info.name + "'" + where + " はまだどのパスも書いていないので、load しても中身は不定です (clear か dontCare にしてください)");
							else
								error(u.path, "'" + info.name + "'" + where + " はどのパスも書いていないのに読んでいます (" + m_passes[ei].name + " より前に書くパスが必要です)");
						}
					}
					if (u.write)
					{
						for (uint32_t r : readers[sub])
							addEdge(r, ei, false);
						if (writer >= 0)
							addEdge(static_cast<uint32_t>(writer), ei, false);
						lastWriter[sub] = static_cast<int32_t>(ei);
						readers[sub].clear();
					}
					else
						readers[sub].push_back(ei);
				}
			}
			m_lastWriters[ri] = lastWriter;
		}

		// 明示的な依存 (after)。元の名前を書くと、すべての繰り返しのあとになる
		for (uint32_t ei = 0; ei < m_passes.size(); ++ei)
		{
			const PassDesc& p = m_passes[ei].d;
			for (size_t i = 0; i < p.after.size(); ++i)
			{
				const std::string ap = m_passes[ei].path + ".after[" + std::to_string(i) + "]";
				std::vector<uint32_t> targets;
				if (auto it = m_passByName.find(p.after[i]); it != m_passByName.end())
					targets.push_back(it->second);
				else if (auto bt = m_byBase.find(p.after[i]); bt != m_byBase.end())
					targets = bt->second;
				else if (m_baseNames.count(p.after[i]))
				{
					warning(ap, "パス '" + p.after[i] + "' は実行しない (無効 / 条件が偽) ので無視します");
					continue;
				}
				else
				{
					error(ap, "パス '" + p.after[i] + "' はありません");
					continue;
				}
				for (uint32_t t : targets)
				{
					if (t == ei) { error(ap, "自分自身のあとには実行できません"); continue; }
					addEdge(t, ei, false);
				}
			}
		}
	}

	// ---- 5. カリング ----
	void cull()
	{
		std::vector<uint32_t> stack(m_roots.begin(), m_roots.end());
		bool changed = true;
		while (changed)
		{
			changed = false;
			while (!stack.empty())
			{
				const uint32_t e = stack.back();
				stack.pop_back();
				if (!m_needed.insert(e).second)
					continue;
				for (uint32_t q : m_rawPred[e])
					stack.push_back(q);
			}
			// 残るパスが name@prev を読むなら、次のフレームのために name を最後に書くパスも残す
			for (uint32_t e : std::vector<uint32_t>(m_needed.begin(), m_needed.end()))
				for (const auto& [ri, subs] : m_uses[e])
				{
					const int32_t base = m_out.resources[ri].historyOf;
					if (base < 0) continue;
					for (const auto& [sub, w] : m_lastWriters[static_cast<uint32_t>(base)])
						if (!m_needed.count(static_cast<uint32_t>(w)))
						{
							stack.push_back(static_cast<uint32_t>(w));
							changed = true;
						}
				}
		}
		for (uint32_t e = 0; e < m_passes.size(); ++e)
			if (!m_needed.count(e))
				m_out.skipped.push_back({ m_passes[e].name, m_passes[e].descIndex, SkippedPass::Reason::Culled });
		if (m_roots.empty() && !m_passes.empty())
			warning("passes", "グラフの結果 ($ で始まるリソースへの出力、または keep のパス) がないので、すべてのパスを除きました");
	}

	// ---- 6. 順序 (トポロジカルソート) ----
	bool sortPasses()
	{
		std::map<uint32_t, uint32_t> indegree;
		for (uint32_t e : m_needed)
		{
			uint32_t n = 0;
			for (uint32_t q : m_pred[e])
				if (m_needed.count(q)) ++n;
			indegree[e] = n;
		}
		// order の小さい順、同じなら宣言順 (展開した順)
		auto later = [this](uint32_t a, uint32_t b) {
			const int32_t oa = m_passes[a].d.order, ob = m_passes[b].d.order;
			return oa != ob ? oa > ob : a > b;
		};
		std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(later)> ready(later);
		for (const auto& [e, n] : indegree)
			if (n == 0) ready.push(e);

		while (!ready.empty())
		{
			const uint32_t e = ready.top();
			ready.pop();
			m_sorted.push_back(e);
			for (uint32_t q : m_succ[e])
			{
				auto it = indegree.find(q);
				if (it != indegree.end() && --it->second == 0)
					ready.push(q);
			}
		}
		if (m_sorted.size() != m_needed.size())
		{
			std::string names;
			for (const auto& [e, n] : indegree)
				if (n > 0)
					names += (names.empty() ? "" : ", ") + m_passes[e].name;
			error("passes", "パスの依存が循環しています (" + names + ")。after の指定を確認してください");
			return false;
		}
		return true;
	}

	// ---- 7. 寿命とバリア ----
	void simulate()
	{
		struct Track
		{
			bool                          started  = false;
			std::vector<GfxResourceState> states;           // サブリソースごと
			bool                          uavWrite = false;  // 直前に使ったパスが UAV で書いた
		};
		std::vector<Track> track(m_out.resources.size());

		for (size_t k = 0; k < m_sorted.size(); ++k)
		{
			const uint32_t ei = m_sorted[k];
			CompiledPass cp;
			cp.passIndex = m_passes[ei].descIndex;
			cp.name      = m_passes[ei].name;
			cp.iteration = m_passes[ei].iteration;
			cp.desc      = m_passes[ei].d;

			for (const auto& [ri, subs] : m_uses[ei])
			{
				ResourceInfo& info = m_out.resources[ri];
				Track&        t    = track[ri];
				const uint32_t total = info.mips * info.layers;
				if (info.firstPass < 0) info.firstPass = static_cast<int32_t>(k);
				info.lastPass = static_cast<int32_t>(k);

				bool anyWrite = false, anyUav = false, uavWriteNow = false;
				for (const auto& [sub, u] : subs)
				{
					anyWrite    |= u.write;
					anyUav      |= u.state == GfxResourceState::UnorderedAccess;
					uavWriteNow |= u.uav && u.write;
				}
				if (anyWrite) info.written = true;

				// 最初に使うとき
				const bool trackedStart = info.imported() || info.isPrev()
				                       || (info.descIndex >= 0 && m_g.resources[static_cast<size_t>(info.descIndex)].history);
				if (!t.started)
				{
					t.started = true;
					const GfxResourceState first = subs.begin()->second.state;
					t.states.assign(total, first);
					if (trackedStart)
					{
						// インポートと履歴は、エンジンの状態追跡の値から遷移する (サブリソースは 1 つ)
						cp.barriers.push_back({ ri, GfxBarrierType::Transition, GfxResourceState::Tracked, first, GFX_ALL_SUBRESOURCES });
					}
					else
						info.initialState = first;   // 一時リソースは、最初に使う状態で作る
				}
				else if (anyUav && t.uavWrite)
				{
					// 前のパスが UAV で書いたものを、続けて UAV で使う
					bool allUav = true;
					for (const auto& [sub, u] : subs)
						allUav &= (u.state == GfxResourceState::UnorderedAccess && t.states[sub] == GfxResourceState::UnorderedAccess);
					if (allUav)
						cp.barriers.push_back({ ri, GfxBarrierType::UAV, GfxResourceState::UnorderedAccess, GfxResourceState::UnorderedAccess, GFX_ALL_SUBRESOURCES });
				}

				// 状態遷移: すべてのサブリソースを同じ状態から同じ状態にするなら 1 つにまとめる
				bool uniformBefore = true, uniformAfter = true;
				const GfxResourceState b0 = t.states[subs.begin()->first];
				const GfxResourceState a0 = subs.begin()->second.state;
				for (const auto& [sub, u] : subs)
				{
					uniformBefore &= t.states[sub] == b0;
					uniformAfter  &= u.state == a0;
				}
				bool restUniform = true;
				for (uint32_t s = 0; s < total; ++s)
					restUniform &= t.states[s] == b0;
				if (subs.size() == total && uniformBefore && uniformAfter && restUniform)
				{
					if (b0 != a0)
						cp.barriers.push_back({ ri, GfxBarrierType::Transition, b0, a0, GFX_ALL_SUBRESOURCES });
				}
				else
				{
					for (const auto& [sub, u] : subs)
						if (t.states[sub] != u.state)
							cp.barriers.push_back({ ri, GfxBarrierType::Transition, t.states[sub], u.state, sub });
				}
				for (const auto& [sub, u] : subs)
					t.states[sub] = u.state;
				t.uavWrite = uavWriteNow;
			}
			m_out.passes.push_back(std::move(cp));
		}

		for (size_t ri = 0; ri < m_out.resources.size(); ++ri)
		{
			ResourceInfo& info = m_out.resources[ri];
			const Track&  t    = track[ri];
			if (!t.states.empty())
			{
				info.finalState   = t.states[0];
				info.finalUniform = std::all_of(t.states.begin(), t.states.end(), [&](GfxResourceState s) { return s == t.states[0]; });
			}
			if (!info.used() && info.descIndex >= 0 && !info.isPrev())
				warning("resources." + info.name, "使われていません (除いたパスだけが使っている場合も含む)");
			if (info.isPrev() && info.used())
			{
				const ResourceInfo& base = m_out.resources[static_cast<size_t>(info.historyOf)];
				if (!base.written)
					warning("resources." + base.name, "前フレームの内容 (" + info.name + ") を読みますが、このフレームでどのパスも '" + base.name + "' を書いていません");
			}
		}
	}

	CompiledGraph finish()
	{
		m_out.ok = m_out.errors().empty();
		return std::move(m_out);
	}

	const GraphDesc&      m_g;
	const CompileOptions& m_opt;
	CompiledGraph         m_out;

	std::map<std::string, Parameter>                    m_params;
	std::unordered_map<std::string, uint32_t>           m_resIndex;
	std::vector<ExpPass>                                m_passes;       // 展開したパス (宣言順)
	std::unordered_map<std::string, uint32_t>           m_passByName;   // 展開した名前 → m_passes の位置
	std::unordered_map<std::string, std::vector<uint32_t>> m_byBase;    // 元の名前 → 展開したパス
	std::set<std::string>                               m_baseNames;    // 宣言されたすべてのパスの名前
	std::vector<std::map<uint32_t, std::map<uint32_t, SubUse>>> m_uses; // パス → リソース → サブリソース → 使い方
	std::map<uint32_t, std::map<uint32_t, int32_t>>     m_lastWriters;  // リソース → サブリソース → 最後に書くパス
	std::map<uint32_t, std::set<uint32_t>>              m_succ, m_pred, m_rawPred;
	std::set<uint32_t>                                  m_roots, m_needed;
	std::vector<uint32_t>                               m_sorted;
};
} // namespace

CompiledGraph compile(const GraphDesc& graph, const CompileOptions& options)
{
	return Compiler(graph, options).run();
}
} // namespace gfx::rg
