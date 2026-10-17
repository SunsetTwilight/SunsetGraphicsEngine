// ============================================================
//  engine/rendergraph/src/Compiler.cpp
// ============================================================
#include "gfx/rendergraph/Compiler.h"

#include <algorithm>
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

bool CompiledGraph::isCulled(uint32_t passIndex) const
{
	return std::find(culled.begin(), culled.end(), passIndex) != culled.end();
}

// ============================================================
//  コンパイル
// ============================================================
namespace
{
bool isInputAccess(Access a)
{
	return a == Access::SRV || a == Access::UAV || a == Access::DSVRead || a == Access::CopySrc || a == Access::IndirectArgs;
}
bool isOutputAccess(Access a)
{
	return a == Access::RTV || a == Access::DSV || a == Access::UAV || a == Access::CopyDst;
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

// 1 つのパスでの、1 つのリソースの使い方をまとめたもの
struct PassUse
{
	bool             read  = false;
	bool             write = false;
	bool             uav   = false;            // UAV として使う
	GfxResourceState state = GfxResourceState::Common;
	std::vector<std::string> accesses;         // 診断用 ("SRV", "RTV0" など)
	std::string      path;                     // 最初に参照した場所
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
		collectResources();
		collectPasses();
		for (uint32_t p : m_enabled)
			analyzePass(p);
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
	static std::string passPath(const PassDesc& p) { return "passes[" + p.name + "]"; }

	// ---- 1. リソース ----
	void collectResources()
	{
		for (size_t i = 0; i < m_g.resources.size(); ++i)
		{
			const ResourceDesc& r = m_g.resources[i];
			const std::string path = "resources." + r.name;
			if (r.name.empty()) { error("resources", "名前のないリソースがあります"); continue; }
			if (m_resIndex.count(r.name)) { error(path, "同じ名前のリソースがすでにあります"); continue; }
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
				if (r.clear.hasDepth && !isDepthFormat(r.format))
					warning(path + ".clear.depth", "深度のフォーマットではないので、深度のクリア値は使いません");
			}
			if (r.kind == ResourceKind::Buffer && r.byteSize == 0)
				error(path + ".bytes", "バッファの大きさがありません");

			ResourceInfo info;
			info.name      = r.name;
			info.kind      = r.kind;
			info.descIndex = static_cast<int32_t>(i);
			m_resIndex[r.name] = static_cast<uint32_t>(m_out.resources.size());
			m_out.resources.push_back(info);
		}
	}

	// 参照されたリソースの位置 (宣言していないインポートはここで加える)。見つからなければ -1
	int32_t resolve(const std::string& name, const std::string& path)
	{
		auto it = m_resIndex.find(name);
		if (it != m_resIndex.end())
			return static_cast<int32_t>(it->second);
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

	// ---- 2. パス ----
	void collectPasses()
	{
		std::set<std::string> names;
		for (size_t i = 0; i < m_g.passes.size(); ++i)
		{
			const PassDesc& p = m_g.passes[i];
			if (p.name.empty())
			{
				error("passes[" + std::to_string(i) + "]", "名前のないパスがあります");
				continue;
			}
			if (!names.insert(p.name).second)
			{
				error(passPath(p), "同じ名前のパスがすでにあります");
				continue;
			}
			m_passByName[p.name] = static_cast<uint32_t>(i);
			if (p.enabled)
				m_enabled.push_back(static_cast<uint32_t>(i));
			else
				m_out.disabled.push_back(static_cast<uint32_t>(i));
		}
		m_uses.resize(m_g.passes.size());
	}

	void analyzePass(uint32_t pi)
	{
		const PassDesc&   p    = m_g.passes[pi];
		const std::string path = passPath(p);
		auto& uses = m_uses[pi];   // リソース → このパスでの使い方

		// ---- パイプラインと内容 ----
		if (p.type != PassType::Copy && p.pipeline.empty())
			error(path + ".pipeline", "パイプラインのアセットがありません");
		if (p.type == PassType::Copy && !p.pipeline.empty())
			warning(path + ".pipeline", "Copy のパスはパイプラインを使わないので無視します");
		const ContentKind ck = p.content.kind;
		if (p.type == PassType::Raster && ck == ContentKind::Dispatch)
			error(path + ".content", "Raster のパスでは dispatch できません");
		if (p.type == PassType::Compute && (ck == ContentKind::DrawList || ck == ContentKind::Fullscreen))
			error(path + ".content", "Compute のパスでは描画できません (dispatch か custom)");
		if (p.type != PassType::Copy && ck == ContentKind::None)
			warning(path + ".content", "何も描きません (content がありません)");
		if (p.type == PassType::Copy && ck != ContentKind::None && ck != ContentKind::Custom)
			warning(path + ".content", "Copy のパスでは content を使いません");

		// ---- 入力と出力 ----
		std::set<uint32_t> rtvIndices;
		bool hasDSV = false, hasTarget = false;

		auto use = [&](const PassResourceRef& r, const std::string& refPath, bool output) {
			if (r.res.empty()) { error(refPath + ".res", "リソースの名前がありません"); return; }
			const int32_t ri = resolve(r.res, refPath + ".res");
			if (ri < 0) return;
			const ResourceInfo& info = m_out.resources[static_cast<size_t>(ri)];
			const ResourceDesc* rd   = descOf(static_cast<uint32_t>(ri));
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
			// パスの種類と使い方
			const bool copyAccess = r.as == Access::CopySrc || r.as == Access::CopyDst;
			if (p.type == PassType::Copy && !copyAccess)
			{
				error(refPath + ".as", "Copy のパスでは CopySrc / CopyDst だけを使えます");
				return;
			}
			if (p.type != PassType::Copy && copyAccess)
			{
				error(refPath + ".as", "CopySrc / CopyDst は Copy のパスで使います");
				return;
			}
			if (p.type == PassType::Compute && (r.as == Access::RTV || r.as == Access::DSV || r.as == Access::DSVRead))
			{
				error(refPath + ".as", "Compute のパスでは RTV / DSV を使えません");
				return;
			}
			// リソースの種類とフォーマット
			if (isBuffer && (r.as == Access::RTV || r.as == Access::DSV || r.as == Access::DSVRead))
				error(refPath + ".as", "バッファは " + refName(r) + " に使えません");
			if (isTexture && r.as == Access::IndirectArgs)
				error(refPath + ".as", "間接描画の引数にはバッファが必要です");
			if (isTexture && r.as == Access::RTV && isDepthFormat(rd->format))
				error(refPath + ".as", "深度のフォーマットのテクスチャは RTV に使えません (DSV を使ってください)");
			if (isTexture && (r.as == Access::DSV || r.as == Access::DSVRead) && !isDepthFormat(rd->format))
				error(refPath + ".as", "DSV には深度のフォーマットのテクスチャが必要です");
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

			// 読み書きの記録
			const bool write = output;
			const bool read  = !output || (r.load == LoadOp::Load && r.as != Access::CopyDst);
			PassUse& u = uses[static_cast<uint32_t>(ri)];
			if (u.path.empty()) u.path = refPath;
			const bool wasUsed = !u.accesses.empty();
			const bool uav     = r.as == Access::UAV;
			if (wasUsed)
			{
				// 同じリソースを同じパスで 2 回使う: UAV を入力と出力に書いた場合だけ許す
				const bool bothUav = u.uav && uav && (u.write != write);
				if (!bothUav && (write || u.write || u.uav != uav))
				{
					error(refPath, "リソース '" + r.res + "' を同じパスで " + u.accesses.front() + " と " + refName(r)
					             + " に使っています (読みながら書くことはできません)");
					return;
				}
			}
			u.accesses.push_back(refName(r));
			u.uav   = uav;
			u.read  = u.read || read;
			u.write = u.write || write;
			const GfxResourceState st = requiredState(p.type, r.as);
			u.state = write ? st : (wasUsed ? (u.state | st) : st);
			if (write && info.imported())
				m_roots.insert(pi);
		};

		for (size_t i = 0; i < p.inputs.size(); ++i)
			use(p.inputs[i], path + ".inputs[" + std::to_string(i) + "]", false);
		for (size_t i = 0; i < p.outputs.size(); ++i)
			use(p.outputs[i], path + ".outputs[" + std::to_string(i) + "]", true);

		if (p.type == PassType::Raster && !hasTarget)
			error(path + ".outputs", "Raster のパスには RTV か DSV の出力が必要です");
		if (p.keep)
			m_roots.insert(pi);

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

		validateBindings(pi);
	}

	// ---- bind: 重なりと、バインディングレイアウトとの照合 ----
	void validateBindings(uint32_t pi)
	{
		const PassDesc&   p    = m_g.passes[pi];
		const std::string path = passPath(p);
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

	// ---- 3. 依存 ----
	void addEdge(uint32_t from, uint32_t to, bool raw)
	{
		if (from == to) return;
		m_succ[from].insert(to);
		m_pred[to].insert(from);
		if (raw) m_rawPred[to].insert(from);
	}

	void buildDependencies()
	{
		// リソースごとに、宣言順でたどる
		for (uint32_t ri = 0; ri < m_out.resources.size(); ++ri)
		{
			int32_t lastWriter = -1;
			std::vector<uint32_t> readers;
			for (uint32_t pi : m_enabled)
			{
				auto it = m_uses[pi].find(ri);
				if (it == m_uses[pi].end())
					continue;
				const PassUse& u = it->second;
				if (u.read)
				{
					if (lastWriter >= 0)
						addEdge(static_cast<uint32_t>(lastWriter), pi, true);
					else if (!m_out.resources[ri].imported())
					{
						const PassDesc& p = m_g.passes[pi];
						if (u.write)
							warning(u.path + ".load", "'" + m_out.resources[ri].name + "' はまだどのパスも書いていないので、load しても中身は不定です (clear か dontCare にしてください)");
						else
							error(u.path, "'" + m_out.resources[ri].name + "' はどのパスも書いていないのに読んでいます (" + p.name + " より前に書くパスが必要です)");
					}
				}
				if (u.write)
				{
					for (uint32_t r : readers)
						addEdge(r, pi, false);
					if (lastWriter >= 0)
						addEdge(static_cast<uint32_t>(lastWriter), pi, false);
					lastWriter = static_cast<int32_t>(pi);
					readers.clear();
				}
				else
					readers.push_back(pi);
			}
		}

		// 明示的な依存 (after)
		for (uint32_t pi : m_enabled)
		{
			const PassDesc& p = m_g.passes[pi];
			for (size_t i = 0; i < p.after.size(); ++i)
			{
				const std::string ap = passPath(p) + ".after[" + std::to_string(i) + "]";
				auto it = m_passByName.find(p.after[i]);
				if (it == m_passByName.end()) { error(ap, "パス '" + p.after[i] + "' はありません"); continue; }
				if (it->second == pi)         { error(ap, "自分自身のあとには実行できません"); continue; }
				if (!m_g.passes[it->second].enabled)
				{
					warning(ap, "パス '" + p.after[i] + "' は無効 (enabled: false) なので無視します");
					continue;
				}
				addEdge(it->second, pi, false);
			}
		}
	}

	// ---- 4. カリング ----
	void cull()
	{
		std::vector<uint32_t> stack(m_roots.begin(), m_roots.end());
		while (!stack.empty())
		{
			const uint32_t p = stack.back();
			stack.pop_back();
			if (!m_needed.insert(p).second)
				continue;
			for (uint32_t q : m_rawPred[p])
				stack.push_back(q);
		}
		for (uint32_t pi : m_enabled)
			if (!m_needed.count(pi))
				m_out.culled.push_back(pi);
		if (m_roots.empty() && !m_enabled.empty())
			warning("passes", "グラフの結果 ($ で始まるリソースへの出力、または keep のパス) がないので、すべてのパスを除きました");
	}

	// ---- 5. 順序 (トポロジカルソート) ----
	bool sortPasses()
	{
		std::map<uint32_t, uint32_t> indegree;
		for (uint32_t p : m_needed)
		{
			uint32_t n = 0;
			for (uint32_t q : m_pred[p])
				if (m_needed.count(q)) ++n;
			indegree[p] = n;
		}
		// order の小さい順、同じなら宣言順
		auto later = [this](uint32_t a, uint32_t b) {
			const int32_t oa = m_g.passes[a].order, ob = m_g.passes[b].order;
			return oa != ob ? oa > ob : a > b;
		};
		std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(later)> ready(later);
		for (const auto& [p, n] : indegree)
			if (n == 0) ready.push(p);

		while (!ready.empty())
		{
			const uint32_t p = ready.top();
			ready.pop();
			m_sorted.push_back(p);
			for (uint32_t q : m_succ[p])
			{
				auto it = indegree.find(q);
				if (it != indegree.end() && --it->second == 0)
					ready.push(q);
			}
		}
		if (m_sorted.size() != m_needed.size())
		{
			std::string names;
			for (const auto& [p, n] : indegree)
				if (n > 0)
					names += (names.empty() ? "" : ", ") + m_g.passes[p].name;
			error("passes", "パスの依存が循環しています (" + names + ")。after の指定を確認してください");
			return false;
		}
		return true;
	}

	// ---- 6. 寿命とバリア ----
	void simulate()
	{
		struct Track { bool started = false; GfxResourceState state = GfxResourceState::Common; bool uavWrite = false; };
		std::vector<Track> track(m_out.resources.size());

		for (size_t k = 0; k < m_sorted.size(); ++k)
		{
			const uint32_t pi = m_sorted[k];
			CompiledPass cp;
			cp.passIndex = pi;
			cp.name      = m_g.passes[pi].name;

			for (const auto& [ri, u] : m_uses[pi])
			{
				ResourceInfo& info = m_out.resources[ri];
				Track&        t    = track[ri];
				if (info.firstPass < 0) info.firstPass = static_cast<int32_t>(k);
				info.lastPass = static_cast<int32_t>(k);
				if (u.write) info.written = true;

				const GfxResourceState req = u.state;
				if (!t.started)
				{
					t.started = true;
					if (info.imported())
						cp.barriers.push_back({ ri, GfxBarrierType::Transition, GfxResourceState::Tracked, req });
					else
						info.initialState = req;   // 一時リソースは、最初に使う状態で作る
				}
				else if (t.state != req)
					cp.barriers.push_back({ ri, GfxBarrierType::Transition, t.state, req });
				else if (req == GfxResourceState::UnorderedAccess && (t.uavWrite || u.write))
					cp.barriers.push_back({ ri, GfxBarrierType::UAV, req, req });   // 続けて UAV で書く / 書いたあとを読む
				t.state    = req;
				t.uavWrite = u.uav && u.write;
			}
			m_out.passes.push_back(std::move(cp));
		}
		for (size_t ri = 0; ri < m_out.resources.size(); ++ri)
		{
			m_out.resources[ri].finalState = track[ri].state;
			const ResourceInfo& info = m_out.resources[ri];
			if (!info.used() && info.descIndex >= 0)
				warning("resources." + info.name, "使われていません (除いたパスだけが使っている場合も含む)");
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

	std::unordered_map<std::string, uint32_t> m_resIndex;
	std::unordered_map<std::string, uint32_t> m_passByName;
	std::vector<uint32_t>                     m_enabled;
	std::vector<std::map<uint32_t, PassUse>>  m_uses;    // パス → (リソース → 使い方)
	std::map<uint32_t, std::set<uint32_t>>    m_succ, m_pred, m_rawPred;
	std::set<uint32_t>                        m_roots, m_needed;
	std::vector<uint32_t>                     m_sorted;
};
} // namespace

CompiledGraph compile(const GraphDesc& graph, const CompileOptions& options)
{
	return Compiler(graph, options).run();
}
} // namespace gfx::rg
