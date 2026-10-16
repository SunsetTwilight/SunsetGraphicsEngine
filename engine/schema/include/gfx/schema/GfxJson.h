#pragma once
// ============================================================
//  gfx/schema/GfxJson.h
//
//  記述子 (スキーマに登録した構造体) と JSON の相互変換。外部ライブラリに依存しない小さな実装。
//
//    GfxJson                 JSON の値 (null / bool / 数値 / 文字列 / 配列 / オブジェクト)
//    gfxJsonParse            文字列 → GfxJson
//    gfxJsonStringify        GfxJson → 文字列
//    gfxToJson / gfxFromJson 記述子 ⇔ GfxJson (スキーマの表をたどる汎用の変換)
//    gfxDescribeStruct       構造体のフィールド表を JSON にする (エディターのフォーム生成用)
//
//  変換の規則:
//    ・列挙型は列挙子の名前 ("Back")、ビットフラグは名前の配列 (["Red", "Green"])。
//      表にない値は数値のまま書く。読むときは数値も受け付ける。
//    ・固定長の文字列 (char[N]) は文字列、固定長配列は配列。
//    ・既定では、既定値と同じフィールドは書かない (配列は末尾の既定値の要素を省く)。
//      読むときは既定値で初期化した構造体に、書かれているフィールドだけを上書きする。
//    ・structSize は書かない (読むときは sizeof のまま)。version は書く。
//    ・const char* / ポインタ (debugName など) は書かない。
//    ・ハンドル (ShaderHandle など) は実行時の値なので、既定では書かない。
//      アセットの参照 (名前やパス) に置き換えるときは GfxJsonWriteOptions::writeHandle /
//      GfxJsonReadOptions::readHandle を使う。
//    ・float は float として最短で往復できる表記で書く (0.1f → 0.1)。inf / nan は文字列。
//
//  使い方:
//    GfxGraphicsPipelineDesc desc = ...;
//    std::string text = gfxJsonStringify(gfxToJson(desc));
//
//    GfxJson json; std::string err;
//    if (gfxJsonParse(text, json, &err)) {
//        GfxGraphicsPipelineDesc loaded;          // 既定値から始める
//        GfxJsonDiagnostics diag;
//        if (gfxFromJson(json, loaded, &diag)) { ... }
//    }
// ============================================================

#include "gfx/schema/GfxSchema.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// ============================================================
//  GfxJson (JSON の値)
// ============================================================
class GfxJson
{
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	GfxJson() = default;
	GfxJson(std::nullptr_t)       {}
	GfxJson(bool v)               : m_type(Type::Bool),   m_bool(v)   {}
	GfxJson(double v)             : m_type(Type::Number), m_number(v) {}
	GfxJson(int v)                : m_type(Type::Number), m_number(v) {}
	GfxJson(int64_t v)            : m_type(Type::Number), m_number(static_cast<double>(v)) {}
	GfxJson(uint32_t v)           : m_type(Type::Number), m_number(v) {}
	GfxJson(uint64_t v)           : m_type(Type::Number), m_number(static_cast<double>(v)) {}
	GfxJson(const char* v)        : m_type(Type::String), m_string(v ? v : "") {}
	GfxJson(std::string v)        : m_type(Type::String), m_string(std::move(v)) {}
	GfxJson(std::string_view v)   : m_type(Type::String), m_string(v) {}

	static GfxJson array()  { GfxJson j; j.m_type = Type::Array;  return j; }
	static GfxJson object() { GfxJson j; j.m_type = Type::Object; return j; }

	Type type()      const { return m_type; }
	bool isNull()    const { return m_type == Type::Null;   }
	bool isBool()    const { return m_type == Type::Bool;   }
	bool isNumber()  const { return m_type == Type::Number; }
	bool isString()  const { return m_type == Type::String; }
	bool isArray()   const { return m_type == Type::Array;  }
	bool isObject()  const { return m_type == Type::Object; }

	bool               asBool()   const { return m_bool;   }
	double             asNumber() const { return m_number; }
	const std::string& asString() const { return m_string; }

	// ---- 配列 ----
	size_t         size() const { return m_type == Type::Object ? m_keys.size() : m_items.size(); }
	const GfxJson& at(size_t i) const { return m_items[i]; }
	GfxJson&       at(size_t i)       { return m_items[i]; }
	GfxJson&       push(GfxJson v)    { m_type = Type::Array; m_items.push_back(std::move(v)); return m_items.back(); }
	const std::vector<GfxJson>& items() const { return m_items; }

	// ---- オブジェクト (キーの順序を保つ) ----
	const std::vector<std::string>& keys() const { return m_keys; }
	const GfxJson& valueAt(size_t i) const { return m_items[i]; }
	const GfxJson* find(std::string_view key) const
	{
		for (size_t i = 0; i < m_keys.size(); ++i)
			if (m_keys[i] == key)
				return &m_items[i];
		return nullptr;
	}
	// 同じキーがあれば置き換え、なければ末尾に追加する
	GfxJson& set(std::string_view key, GfxJson v)
	{
		m_type = Type::Object;
		for (size_t i = 0; i < m_keys.size(); ++i)
			if (m_keys[i] == key)
				return m_items[i] = std::move(v);
		m_keys.emplace_back(key);
		m_items.push_back(std::move(v));
		return m_items.back();
	}
	const GfxJson& operator[](std::string_view key) const
	{
		static const GfxJson null;
		const GfxJson* p = find(key);
		return p ? *p : null;
	}

private:
	Type                     m_type   = Type::Null;
	bool                     m_bool   = false;
	double                   m_number = 0.0;
	std::string              m_string;
	std::vector<GfxJson>     m_items;   // 配列の要素 / オブジェクトの値
	std::vector<std::string> m_keys;    // オブジェクトのキー
};

// ============================================================
//  文字列化
// ============================================================
namespace gfx_json_detail
{
inline void appendNumber(std::string& out, double v)
{
	if (!std::isfinite(v))
	{
		out += "null";   // JSON は inf / nan を表せない (記述子の変換では文字列にする)
		return;
	}
	// 2^53 未満の整数は整数として書く
	if (std::fabs(v) < 9007199254740992.0 && v == std::floor(v))
	{
		char buf[32];
		const auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(v));
		out.append(buf, r.ptr);
		return;
	}
	char buf[64];
	const auto r = std::to_chars(buf, buf + sizeof(buf), v);   // 最短で往復できる表記
	out.append(buf, r.ptr);
}

inline void appendString(std::string& out, const std::string& s)
{
	static const char* hex = "0123456789abcdef";
	out += '"';
	for (unsigned char c : s)
	{
		switch (c)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b";  break;
		case '\f': out += "\\f";  break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (c < 0x20)
			{
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
			else
				out += static_cast<char>(c);   // UTF-8 はそのまま
		}
	}
	out += '"';
}

// 要素がすべてスカラーの配列は 1 行に書く
inline bool isFlatArray(const GfxJson& v)
{
	for (const auto& e : v.items())
		if (e.isArray() || e.isObject())
			return false;
	return true;
}

inline void stringify(std::string& out, const GfxJson& v, int indent, int depth)
{
	auto newline = [&](int d) {
		if (indent <= 0) return;
		out += '\n';
		out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
	};
	switch (v.type())
	{
	case GfxJson::Type::Null:   out += "null"; break;
	case GfxJson::Type::Bool:   out += v.asBool() ? "true" : "false"; break;
	case GfxJson::Type::Number: appendNumber(out, v.asNumber()); break;
	case GfxJson::Type::String: appendString(out, v.asString()); break;
	case GfxJson::Type::Array:
	{
		if (v.size() == 0) { out += "[]"; break; }
		const bool flat = isFlatArray(v);
		out += '[';
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (i > 0) out += flat && indent > 0 ? ", " : ",";
			if (!flat) newline(depth + 1);
			stringify(out, v.at(i), indent, depth + 1);
		}
		if (!flat) newline(depth);
		out += ']';
		break;
	}
	case GfxJson::Type::Object:
	{
		if (v.size() == 0) { out += "{}"; break; }
		out += '{';
		for (size_t i = 0; i < v.keys().size(); ++i)
		{
			if (i > 0) out += ',';
			newline(depth + 1);
			appendString(out, v.keys()[i]);
			out += indent > 0 ? ": " : ":";
			stringify(out, v.valueAt(i), indent, depth + 1);
		}
		newline(depth);
		out += '}';
		break;
	}
	}
}
} // namespace gfx_json_detail

// indent = 0 なら 1 行で書く
inline std::string gfxJsonStringify(const GfxJson& v, int indent = 2)
{
	std::string out;
	gfx_json_detail::stringify(out, v, indent, 0);
	return out;
}

// ============================================================
//  パース
// ============================================================
namespace gfx_json_detail
{
class Parser
{
public:
	explicit Parser(std::string_view text) : m_s(text) {}

	bool parse(GfxJson& out, std::string* error)
	{
		skipSpace();
		if (!value(out, 0))
			return fail(error);
		skipSpace();
		if (m_pos != m_s.size())
		{
			m_error = "値のあとに余分な文字があります";
			return fail(error);
		}
		return true;
	}

private:
	static constexpr int kMaxDepth = 256;

	bool fail(std::string* error) const
	{
		if (error)
		{
			size_t line = 1, col = 1;
			for (size_t i = 0; i < m_pos && i < m_s.size(); ++i)
			{
				if (m_s[i] == '\n') { ++line; col = 1; }
				else                { ++col; }
			}
			*error = std::to_string(line) + ":" + std::to_string(col) + ": " + m_error;
		}
		return false;
	}

	void skipSpace()
	{
		while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r'))
			++m_pos;
	}

	bool literal(std::string_view word)
	{
		if (m_s.substr(m_pos, word.size()) != word)
		{
			m_error = "不明な値です";
			return false;
		}
		m_pos += word.size();
		return true;
	}

	bool value(GfxJson& out, int depth)
	{
		if (depth > kMaxDepth) { m_error = "入れ子が深すぎます"; return false; }
		if (m_pos >= m_s.size()) { m_error = "値がありません"; return false; }
		const char c = m_s[m_pos];
		if (c == '{') return object(out, depth);
		if (c == '[') return array(out, depth);
		if (c == '"')
		{
			std::string s;
			if (!string(s)) return false;
			out = GfxJson(std::move(s));
			return true;
		}
		if (c == 't') { if (!literal("true"))  return false; out = GfxJson(true);  return true; }
		if (c == 'f') { if (!literal("false")) return false; out = GfxJson(false); return true; }
		if (c == 'n') { if (!literal("null"))  return false; out = GfxJson();      return true; }
		if (c == '-' || (c >= '0' && c <= '9')) return number(out);
		m_error = std::string("予期しない文字 '") + c + "' です";
		return false;
	}

	bool number(GfxJson& out)
	{
		const size_t start = m_pos;
		if (m_s[m_pos] == '-') ++m_pos;
		auto digits = [&]() { size_t n = 0; while (m_pos < m_s.size() && m_s[m_pos] >= '0' && m_s[m_pos] <= '9') { ++m_pos; ++n; } return n; };
		if (digits() == 0) { m_error = "数値が不正です"; return false; }
		if (m_pos < m_s.size() && m_s[m_pos] == '.')
		{
			++m_pos;
			if (digits() == 0) { m_error = "小数点のあとに数字がありません"; return false; }
		}
		if (m_pos < m_s.size() && (m_s[m_pos] == 'e' || m_s[m_pos] == 'E'))
		{
			++m_pos;
			if (m_pos < m_s.size() && (m_s[m_pos] == '+' || m_s[m_pos] == '-')) ++m_pos;
			if (digits() == 0) { m_error = "指数が不正です"; return false; }
		}
		double v = 0.0;
		const char* first = m_s.data() + start;
		const char* last  = m_s.data() + m_pos;
		const auto r = std::from_chars(first, last, v);   // ロケールに依存しない
		if (r.ec != std::errc() || r.ptr != last)
		{
			m_error = "数値が範囲外です";
			m_pos = start;
			return false;
		}
		out = GfxJson(v);
		return true;
	}

	static void appendUtf8(std::string& s, uint32_t cp)
	{
		if (cp < 0x80) s += static_cast<char>(cp);
		else if (cp < 0x800)
		{
			s += static_cast<char>(0xC0 | (cp >> 6));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			s += static_cast<char>(0xE0 | (cp >> 12));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			s += static_cast<char>(0xF0 | (cp >> 18));
			s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(uint32_t& out)
	{
		if (m_pos + 4 > m_s.size()) { m_error = "\\u のあとに 16 進数 4 桁が必要です"; return false; }
		out = 0;
		for (int i = 0; i < 4; ++i)
		{
			const char c = m_s[m_pos++];
			out <<= 4;
			if      (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
			else { m_error = "\\u のあとに 16 進数 4 桁が必要です"; return false; }
		}
		return true;
	}

	bool string(std::string& out)
	{
		++m_pos;   // "
		while (m_pos < m_s.size())
		{
			const char c = m_s[m_pos++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20) { --m_pos; m_error = "文字列に制御文字があります"; return false; }
			if (c != '\\') { out += c; continue; }
			if (m_pos >= m_s.size()) break;
			const char e = m_s[m_pos++];
			switch (e)
			{
			case '"':  out += '"';  break;
			case '\\': out += '\\'; break;
			case '/':  out += '/';  break;
			case 'b':  out += '\b'; break;
			case 'f':  out += '\f'; break;
			case 'n':  out += '\n'; break;
			case 'r':  out += '\r'; break;
			case 't':  out += '\t'; break;
			case 'u':
			{
				uint32_t cp = 0;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF)   // サロゲートペア
				{
					uint32_t lo = 0;
					if (m_pos + 2 > m_s.size() || m_s[m_pos] != '\\' || m_s[m_pos + 1] != 'u') { m_error = "サロゲートペアの後半がありません"; return false; }
					m_pos += 2;
					if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) { m_error = "サロゲートペアが不正です"; return false; }
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				appendUtf8(out, cp);
				break;
			}
			default:
				--m_pos;
				m_error = std::string("不明なエスケープ \\") + e + " です";
				return false;
			}
		}
		m_error = "文字列が閉じていません";
		return false;
	}

	bool array(GfxJson& out, int depth)
	{
		++m_pos;   // [
		out = GfxJson::array();
		skipSpace();
		if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return true; }
		for (;;)
		{
			skipSpace();
			GfxJson item;
			if (!value(item, depth + 1)) return false;
			out.push(std::move(item));
			skipSpace();
			if (m_pos < m_s.size() && m_s[m_pos] == ',') { ++m_pos; continue; }
			if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return true; }
			m_error = "配列に ',' か ']' が必要です";
			return false;
		}
	}

	bool object(GfxJson& out, int depth)
	{
		++m_pos;   // {
		out = GfxJson::object();
		skipSpace();
		if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return true; }
		for (;;)
		{
			skipSpace();
			if (m_pos >= m_s.size() || m_s[m_pos] != '"') { m_error = "オブジェクトのキーは文字列です"; return false; }
			std::string key;
			if (!string(key)) return false;
			skipSpace();
			if (m_pos >= m_s.size() || m_s[m_pos] != ':') { m_error = "キーのあとに ':' が必要です"; return false; }
			++m_pos;
			skipSpace();
			GfxJson item;
			if (!value(item, depth + 1)) return false;
			out.set(key, std::move(item));
			skipSpace();
			if (m_pos < m_s.size() && m_s[m_pos] == ',') { ++m_pos; continue; }
			if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return true; }
			m_error = "オブジェクトに ',' か '}' が必要です";
			return false;
		}
	}

	std::string_view m_s;
	size_t           m_pos = 0;
	std::string      m_error;
};
} // namespace gfx_json_detail

// 失敗したら false。error には "行:列: 理由" を入れる
inline bool gfxJsonParse(std::string_view text, GfxJson& out, std::string* error = nullptr)
{
	GfxJson tmp;
	if (!gfx_json_detail::Parser(text).parse(tmp, error))
		return false;
	out = std::move(tmp);
	return true;
}

// ============================================================
//  記述子 ⇔ JSON
// ============================================================
struct GfxJsonWriteOptions
{
	bool writeDefaults = false;   // 既定値と同じフィールドも書く

	// ハンドルのフィールドを書く (アセットの名前などに置き換える)。null を返したら書かない。
	// 未設定ならハンドルは書かない。
	std::function<GfxJson(const GfxFieldInfo& field, uint64_t id)> writeHandle;
};

struct GfxJsonReadOptions
{
	// ハンドルのフィールドを読む (名前などからハンドルの id を求める)。false なら失敗として扱う。
	// 未設定ならハンドルのフィールドは無視して警告にする。
	std::function<bool(const GfxFieldInfo& field, const GfxJson& value, uint64_t& outId)> readHandle;

	bool unknownFieldIsError = false;   // 知らないキーをエラーにする (既定は警告。新しいファイルを古いエンジンで読むため)
};

// 読み込みの結果 (path は "rasterizerState.cullMode" や "rtvFormats[2]" の形)
struct GfxJsonDiagnostics
{
	std::vector<std::string> errors;
	std::vector<std::string> warnings;
};

namespace gfx_json_detail
{
inline bool isSkipped(const GfxFieldInfo& f)
{
	return f.type == GfxFieldType::CString || f.type == GfxFieldType::Pointer
		|| std::strcmp(f.name, "structSize") == 0;
}

inline size_t charLength(const char* p, uint32_t cap)
{
	size_t n = 0;
	while (n < cap && p[n] != '\0') ++n;
	return n;
}

// 要素 1 個が既定値と同じか (def が nullptr なら既定値と比べない)
inline bool elementEquals(const GfxFieldInfo& f, const void* a, const void* b);

inline bool structEquals(const GfxStructInfo* si, const void* a, const void* b)
{
	for (uint32_t i = 0; i < si->fieldCount; ++i)
	{
		const GfxFieldInfo& f = si->fields[i];
		if (isSkipped(f))
			continue;
		if (f.type == GfxFieldType::Char)
		{
			if (!elementEquals(f, gfxFieldPtr(a, f), gfxFieldPtr(b, f)))
				return false;
			continue;
		}
		for (uint32_t k = 0; k < f.arrayCount; ++k)
			if (!elementEquals(f, gfxFieldPtr(a, f, k), gfxFieldPtr(b, f, k)))
				return false;
	}
	return true;
}

inline bool elementEquals(const GfxFieldInfo& f, const void* a, const void* b)
{
	switch (f.type)
	{
	case GfxFieldType::Struct:
		return f.structInfo ? structEquals(f.structInfo, a, b) : std::memcmp(a, b, f.size) == 0;
	case GfxFieldType::Char:
	{
		const size_t na = charLength(static_cast<const char*>(a), f.arrayCount);
		const size_t nb = charLength(static_cast<const char*>(b), f.arrayCount);
		return na == nb && std::memcmp(a, b, na) == 0;
	}
	case GfxFieldType::Bool:
		return (*static_cast<const bool*>(a)) == (*static_cast<const bool*>(b));
	case GfxFieldType::CString:
	case GfxFieldType::Pointer:
		return true;
	default:
		return std::memcmp(a, b, f.size) == 0;   // 整数 / 列挙 / float (ビット単位) / ハンドル
	}
}

inline GfxJson floatToJson(float v)
{
	if (std::isnan(v)) return GfxJson("nan");
	if (std::isinf(v)) return GfxJson(v > 0 ? "inf" : "-inf");
	// float として最短の表記を double に読み直す (0.1f → 0.1。読むときに同じ float に戻る)
	char buf[32];
	const auto r = std::to_chars(buf, buf + sizeof(buf), v);
	double d = 0.0;
	std::from_chars(buf, r.ptr, d);
	return GfxJson(d);
}

inline GfxJson flagsToJson(const GfxEnumInfo* ei, uint64_t value)
{
	GfxJson arr = GfxJson::array();
	if (!ei)
	{
		arr.push(GfxJson(value));
		return arr;
	}
	// 0 は名前があれば書く (None など)
	if (value == 0)
	{
		return arr;
	}
	// 値ちょうどの名前 (All など) があればそれを使う
	if (const char* exact = gfxEnumName(ei, value))
	{
		arr.push(GfxJson(exact));
		return arr;
	}
	// ビット数の多い列挙子から順に、含まれているものを取り出す
	uint64_t rest = value;
	std::vector<const GfxEnumerator*> order;
	for (uint32_t i = 0; i < ei->valueCount; ++i)
		if (ei->values[i].value != 0)
			order.push_back(&ei->values[i]);
	auto bits = [](uint64_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; };
	std::stable_sort(order.begin(), order.end(), [&](const GfxEnumerator* a, const GfxEnumerator* b) { return bits(a->value) > bits(b->value); });
	std::vector<const GfxEnumerator*> picked;
	for (const GfxEnumerator* e : order)
		if ((rest & e->value) == e->value)
		{
			picked.push_back(e);
			rest &= ~e->value;
		}
	// 表の順に並べ直す
	std::sort(picked.begin(), picked.end());
	for (const GfxEnumerator* e : picked)
		arr.push(GfxJson(e->name));
	if (rest != 0)
		arr.push(GfxJson(rest));
	return arr;
}

inline GfxJson structToJson(const GfxStructInfo* si, const void* obj, const void* def, const GfxJsonWriteOptions& opts);

// 要素 1 個 (スカラー / 構造体)
inline GfxJson elementToJson(const GfxFieldInfo& f, const void* p, const void* def, const GfxJsonWriteOptions& opts)
{
	switch (f.type)
	{
	case GfxFieldType::Bool:   return GfxJson(*static_cast<const bool*>(p));
	case GfxFieldType::UInt8:  return GfxJson(static_cast<uint32_t>(*static_cast<const uint8_t*>(p)));
	case GfxFieldType::Int32:  { int32_t v;  std::memcpy(&v, p, 4); return GfxJson(static_cast<int64_t>(v)); }
	case GfxFieldType::UInt32: { uint32_t v; std::memcpy(&v, p, 4); return GfxJson(v); }
	case GfxFieldType::Float:  { float v;    std::memcpy(&v, p, 4); return floatToJson(v); }
	case GfxFieldType::Enum:
	{
		const uint64_t v = gfxReadUnsigned(p, f.size);
		if (const char* name = gfxEnumName(f.enumInfo, v))
			return GfxJson(name);
		return GfxJson(v);
	}
	case GfxFieldType::Flags:
		return flagsToJson(f.enumInfo, gfxReadUnsigned(p, f.size));
	case GfxFieldType::Struct:
		return f.structInfo ? structToJson(f.structInfo, p, def, opts) : GfxJson();
	case GfxFieldType::Handle:
		return opts.writeHandle ? opts.writeHandle(f, gfxReadUnsigned(p, f.size)) : GfxJson();
	default:
		return GfxJson();
	}
}

inline GfxJson structToJson(const GfxStructInfo* si, const void* obj, const void* def, const GfxJsonWriteOptions& opts)
{
	GfxJson out = GfxJson::object();
	const bool compare = !opts.writeDefaults && def != nullptr;
	for (uint32_t i = 0; i < si->fieldCount; ++i)
	{
		const GfxFieldInfo& f = si->fields[i];
		if (isSkipped(f))
			continue;
		if (f.type == GfxFieldType::Handle && !opts.writeHandle)
			continue;

		const void* fdef = def ? gfxFieldPtr(def, f) : nullptr;

		if (f.type == GfxFieldType::Char)
		{
			if (compare && elementEquals(f, gfxFieldPtr(obj, f), fdef))
				continue;
			const char* s = static_cast<const char*>(gfxFieldPtr(obj, f));
			out.set(f.name, GfxJson(std::string(s, charLength(s, f.arrayCount))));
			continue;
		}

		if (f.arrayCount == 1)
		{
			if (compare && elementEquals(f, gfxFieldPtr(obj, f), fdef))
				continue;
			GfxJson v = elementToJson(f, gfxFieldPtr(obj, f), fdef, opts);
			if (!v.isNull() || f.type != GfxFieldType::Handle)
				out.set(f.name, std::move(v));
			continue;
		}

		// 固定長配列: 末尾の既定値の要素を省く
		uint32_t count = f.arrayCount;
		if (compare)
			while (count > 0 && elementEquals(f, gfxFieldPtr(obj, f, count - 1), gfxFieldPtr(def, f, count - 1)))
				--count;
		if (count == 0)
			continue;
		GfxJson arr = GfxJson::array();
		for (uint32_t k = 0; k < count; ++k)
			arr.push(elementToJson(f, gfxFieldPtr(obj, f, k), def ? gfxFieldPtr(def, f, k) : nullptr, opts));
		out.set(f.name, std::move(arr));
	}
	return out;
}

// ---- 読み込み ----
struct ReadContext
{
	const GfxJsonReadOptions& opts;
	GfxJsonDiagnostics&       diag;
	std::string               path;

	void error(const std::string& msg)   { diag.errors.push_back(path + ": " + msg); }
	void warning(const std::string& msg) { diag.warnings.push_back(path + ": " + msg); }
};

inline bool readInteger(const GfxJson& v, double lo, double hi, ReadContext& ctx, double& out)
{
	if (v.isBool()) { out = v.asBool() ? 1.0 : 0.0; return true; }
	if (!v.isNumber()) { ctx.error("数値が必要です"); return false; }
	const double d = v.asNumber();
	if (d != std::floor(d)) { ctx.error("整数が必要です"); return false; }
	if (d < lo || d > hi)   { ctx.error("値が型の範囲外です"); return false; }
	out = d;
	return true;
}

inline void checkRange(const GfxFieldInfo& f, double v, ReadContext& ctx)
{
	if (f.minValue < f.maxValue && (v < f.minValue || v > f.maxValue))
		ctx.warning("値 " + std::to_string(v) + " が推奨範囲 [" + std::to_string(f.minValue) + ", " + std::to_string(f.maxValue) + "] の外です");
}

inline bool enumFromJson(const GfxEnumInfo* ei, const GfxJson& v, ReadContext& ctx, uint64_t& out)
{
	if (v.isNumber())
	{
		double d = 0;
		if (!readInteger(v, 0, 18446744073709551615.0, ctx, d)) return false;
		out = static_cast<uint64_t>(d);
		if (ei && !gfxEnumName(ei, out))
			ctx.warning("列挙子の表にない値 " + std::to_string(out) + " です");
		return true;
	}
	if (v.isString())
	{
		if (gfxEnumValue(ei, v.asString().c_str(), out))
			return true;
		ctx.error("列挙子 '" + v.asString() + "' は " + (ei ? ei->name : "?") + " にありません");
		return false;
	}
	ctx.error("列挙子の名前 (文字列) が必要です");
	return false;
}

inline bool flagsFromJson(const GfxEnumInfo* ei, const GfxJson& v, ReadContext& ctx, uint64_t& out)
{
	out = 0;
	auto one = [&](const GfxJson& e) -> bool {
		uint64_t bit = 0;
		if (!enumFromJson(ei, e, ctx, bit)) return false;
		out |= bit;
		return true;
	};
	if (v.isArray())
	{
		bool ok = true;
		for (const auto& e : v.items()) ok &= one(e);
		return ok;
	}
	if (v.isString())   // "Red|Green" も受け付ける
	{
		const std::string& s = v.asString();
		bool   ok    = true;
		size_t start = 0;
		while (start <= s.size())
		{
			size_t end = s.find('|', start);
			std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
			while (!part.empty() && part.front() == ' ') part.erase(part.begin());
			while (!part.empty() && part.back()  == ' ') part.pop_back();
			if (!part.empty()) ok &= one(GfxJson(part));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return ok;
	}
	return one(v);   // 数値
}

inline bool structFromJson(const GfxStructInfo* si, const GfxJson& json, void* obj, ReadContext& ctx);

inline bool elementFromJson(const GfxFieldInfo& f, const GfxJson& v, void* p, ReadContext& ctx)
{
	switch (f.type)
	{
	case GfxFieldType::Bool:
		if (v.isBool())   { *static_cast<bool*>(p) = v.asBool(); return true; }
		if (v.isNumber()) { *static_cast<bool*>(p) = v.asNumber() != 0.0; return true; }
		ctx.error("true / false が必要です");
		return false;
	case GfxFieldType::UInt8:
	{
		double d = 0;
		if (!readInteger(v, 0, 255, ctx, d)) return false;
		checkRange(f, d, ctx);
		*static_cast<uint8_t*>(p) = static_cast<uint8_t>(d);
		return true;
	}
	case GfxFieldType::Int32:
	{
		double d = 0;
		if (!readInteger(v, -2147483648.0, 2147483647.0, ctx, d)) return false;
		checkRange(f, d, ctx);
		const int32_t x = static_cast<int32_t>(d);
		std::memcpy(p, &x, 4);
		return true;
	}
	case GfxFieldType::UInt32:
	{
		double d = 0;
		if (!readInteger(v, 0, 4294967295.0, ctx, d)) return false;
		checkRange(f, d, ctx);
		const uint32_t x = static_cast<uint32_t>(d);
		std::memcpy(p, &x, 4);
		return true;
	}
	case GfxFieldType::Float:
	{
		float x = 0.0f;
		if (v.isNumber())
		{
			const double d    = v.asNumber();
			const double fmax = std::numeric_limits<float>::max();
			if (std::fabs(d) <= fmax)
				x = static_cast<float>(d);
			else if (std::fabs(d) <= fmax * (1.0 + 1.0 / 16777216.0))   // FLT_MAX の最短表記 (3.4028235e+38) は少しだけ大きい
				x = static_cast<float>(d > 0 ? fmax : -fmax);
			else
			{
				ctx.error("float の範囲外です");
				return false;
			}
			checkRange(f, d, ctx);
		}
		else if (v.isString() && v.asString() == "inf")  x =  std::numeric_limits<float>::infinity();
		else if (v.isString() && v.asString() == "-inf") x = -std::numeric_limits<float>::infinity();
		else if (v.isString() && v.asString() == "nan")  x =  std::numeric_limits<float>::quiet_NaN();
		else { ctx.error("数値が必要です"); return false; }
		std::memcpy(p, &x, 4);
		return true;
	}
	case GfxFieldType::Enum:
	{
		uint64_t x = 0;
		if (!enumFromJson(f.enumInfo, v, ctx, x)) return false;
		gfxWriteUnsigned(p, f.size, x);
		return true;
	}
	case GfxFieldType::Flags:
	{
		uint64_t x = 0;
		if (!flagsFromJson(f.enumInfo, v, ctx, x)) return false;
		gfxWriteUnsigned(p, f.size, x);
		return true;
	}
	case GfxFieldType::Struct:
		if (!v.isObject()) { ctx.error("オブジェクトが必要です"); return false; }
		return f.structInfo ? structFromJson(f.structInfo, v, p, ctx) : false;
	case GfxFieldType::Handle:
	{
		if (!ctx.opts.readHandle)
		{
			ctx.warning("ハンドルは読み込めないので無視します (GfxJsonReadOptions::readHandle を設定してください)");
			return true;
		}
		uint64_t id = 0;
		if (!ctx.opts.readHandle(f, v, id))
		{
			ctx.error("ハンドルを解決できません");
			return false;
		}
		gfxWriteUnsigned(p, f.size, id);
		return true;
	}
	default:
		return true;
	}
}

inline bool structFromJson(const GfxStructInfo* si, const GfxJson& json, void* obj, ReadContext& ctx)
{
	bool ok = true;
	const std::string base = ctx.path;
	for (size_t i = 0; i < json.keys().size(); ++i)
	{
		const std::string& key = json.keys()[i];
		const GfxJson&     v   = json.valueAt(i);
		ctx.path = base.empty() ? key : base + "." + key;

		const GfxFieldInfo* f = gfxFindField(si, key.c_str());
		if (!f)
		{
			if (ctx.opts.unknownFieldIsError) { ctx.error(std::string(si->name) + " にないフィールドです"); ok = false; }
			else                              { ctx.warning(std::string(si->name) + " にないフィールドなので無視します"); }
			continue;
		}
		if (f->type == GfxFieldType::CString || f->type == GfxFieldType::Pointer)
		{
			ctx.warning("このフィールドは JSON から読み込めないので無視します");
			continue;
		}
		if (std::strcmp(f->name, "structSize") == 0)
			continue;   // sizeof のまま

		if (f->type == GfxFieldType::Char)
		{
			if (!v.isString()) { ctx.error("文字列が必要です"); ok = false; continue; }
			const std::string& s = v.asString();
			if (s.size() + 1 > f->arrayCount)
			{
				ctx.error("文字列が長すぎます (最大 " + std::to_string(f->arrayCount - 1) + " バイト)");
				ok = false;
				continue;
			}
			char* dst = static_cast<char*>(gfxFieldPtr(obj, *f));
			std::memset(dst, 0, f->arrayCount);
			std::memcpy(dst, s.data(), s.size());
			continue;
		}

		if (f->arrayCount == 1)
		{
			ok &= elementFromJson(*f, v, gfxFieldPtr(obj, *f), ctx);
			continue;
		}

		if (!v.isArray()) { ctx.error("配列が必要です"); ok = false; continue; }
		if (v.size() > f->arrayCount)
		{
			ctx.error("要素が多すぎます (最大 " + std::to_string(f->arrayCount) + " 個)");
			ok = false;
			continue;
		}
		const std::string arrayPath = ctx.path;
		for (uint32_t k = 0; k < v.size(); ++k)
		{
			ctx.path = arrayPath + "[" + std::to_string(k) + "]";
			ok &= elementFromJson(*f, v.at(k), gfxFieldPtr(obj, *f, k), ctx);
		}
	}
	ctx.path = base;
	return ok;
}
} // namespace gfx_json_detail

// 構造体 → JSON (オブジェクト)
inline GfxJson gfxStructToJson(const GfxStructInfo* si, const void* obj, const GfxJsonWriteOptions& opts = {})
{
	if (!si || !obj)
		return GfxJson();
	return gfx_json_detail::structToJson(si, obj, si->defaultValue, opts);
}

// JSON → 構造体。obj に書かれているフィールドだけを上書きする (obj は既定値で初期化しておくこと)。
// エラーがあれば false (エラーのあったフィールド以外は書き込まれる)
inline bool gfxStructFromJson(const GfxStructInfo* si, const GfxJson& json, void* obj,
                              GfxJsonDiagnostics* diag = nullptr, const GfxJsonReadOptions& opts = {})
{
	GfxJsonDiagnostics local;
	GfxJsonDiagnostics& d = diag ? *diag : local;
	if (!si || !obj)
	{
		d.errors.push_back("型の情報がありません");
		return false;
	}
	if (!json.isObject())
	{
		d.errors.push_back(std::string(si->name) + ": JSON のオブジェクトが必要です");
		return false;
	}
	gfx_json_detail::ReadContext ctx{ opts, d, {} };
	return gfx_json_detail::structFromJson(si, json, obj, ctx) && d.errors.empty();
}

template<typename T>
GfxJson gfxToJson(const T& value, const GfxJsonWriteOptions& opts = {})
{
	return gfxStructToJson(gfxStructInfo<T>(), &value, opts);
}

// JSON → 記述子。既定値から読み込み、エラーがなかったときだけ out を置き換える
template<typename T>
bool gfxFromJson(const GfxJson& json, T& out, GfxJsonDiagnostics* diag = nullptr, const GfxJsonReadOptions& opts = {})
{
	T tmp{};
	if (!gfxStructFromJson(gfxStructInfo<T>(), json, &tmp, diag, opts))
		return false;
	out = tmp;
	return true;
}

// ============================================================
//  スキーマの説明 (エディターがフォームを作るための JSON)
// ============================================================
inline const char* gfxFieldTypeName(GfxFieldType t)
{
	switch (t)
	{
	case GfxFieldType::Bool:    return "bool";
	case GfxFieldType::UInt8:   return "uint8";
	case GfxFieldType::Int32:   return "int32";
	case GfxFieldType::UInt32:  return "uint32";
	case GfxFieldType::Float:   return "float";
	case GfxFieldType::Enum:    return "enum";
	case GfxFieldType::Flags:   return "flags";
	case GfxFieldType::Struct:  return "struct";
	case GfxFieldType::Char:    return "string";
	case GfxFieldType::Handle:  return "handle";
	case GfxFieldType::CString: return "cstring";
	case GfxFieldType::Pointer: return "pointer";
	default:                    return "unknown";
	}
}

// 列挙型: { "name": "GfxCullMode", "flags": false, "values": [{ "name": "None", "value": 1 }, ...] }
inline GfxJson gfxDescribeEnum(const GfxEnumInfo* ei)
{
	GfxJson out = GfxJson::object();
	if (!ei) return out;
	out.set("name", ei->name);
	out.set("flags", ei->isFlags);
	GfxJson values = GfxJson::array();
	for (uint32_t i = 0; i < ei->valueCount; ++i)
	{
		GfxJson e = GfxJson::object();
		e.set("name", ei->values[i].name);
		e.set("value", ei->values[i].value);
		values.push(std::move(e));
	}
	out.set("values", std::move(values));
	return out;
}

// 構造体: { "name": ..., "size": ..., "fields": [{ "name", "type", "enum" / "struct", "count", "min", "max", "default", "tooltip", "serialized" }] }
inline GfxJson gfxDescribeStruct(const GfxStructInfo* si)
{
	GfxJson out = GfxJson::object();
	if (!si) return out;
	out.set("name", si->name);
	out.set("size", si->size);
	GfxJson fields = GfxJson::array();
	const GfxJsonWriteOptions all{ true, {} };
	for (uint32_t i = 0; i < si->fieldCount; ++i)
	{
		const GfxFieldInfo& f = si->fields[i];
		GfxJson j = GfxJson::object();
		j.set("name", f.name);
		j.set("type", gfxFieldTypeName(f.type));
		if (f.enumInfo)   j.set("enum",   f.enumInfo->name);
		if (f.structInfo) j.set("struct", f.structInfo->name);
		j.set("count", f.arrayCount);
		if (f.minValue < f.maxValue)
		{
			j.set("min", f.minValue);
			j.set("max", f.maxValue);
		}
		const bool serialized = !gfx_json_detail::isSkipped(f) && f.type != GfxFieldType::Handle;
		j.set("serialized", serialized);
		if (serialized && si->defaultValue && f.type != GfxFieldType::Struct)
		{
			if (f.type == GfxFieldType::Char)
			{
				const char* s = static_cast<const char*>(gfxFieldPtr(si->defaultValue, f));
				j.set("default", std::string(s, gfx_json_detail::charLength(s, f.arrayCount)));
			}
			else if (f.arrayCount == 1)
				j.set("default", gfx_json_detail::elementToJson(f, gfxFieldPtr(si->defaultValue, f), nullptr, all));
		}
		if (f.tooltip) j.set("tooltip", f.tooltip);
		fields.push(std::move(j));
	}
	out.set("fields", std::move(fields));
	return out;
}
