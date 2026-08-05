#include "material/material_unpacker.h"

#include "material/spirv_material_reflect.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace velk {

namespace {

struct ScalarType
{
    uint32_t align;
    uint32_t size;
    uint32_t words;   ///< Contiguous 32-bit words the value occupies.
    bool is_float;
};

/// std430 alignment and size for the leaf types velk materials may use.
/// vec3's 16-byte alignment against a 12-byte size is the classic trap; it is
/// modelled here and cross-checked against reflection.
///
/// mat3 is deliberately absent: std430 pads each of its columns to 16 bytes,
/// so its words are not contiguous and the generated reader would be wrong.
/// It falls through to the unsupported-type error instead.
bool leaf_type(string_view t, ScalarType& out)
{
    struct Entry { const char* name; ScalarType info; };
    static const Entry kTypes[] = {
        {"float", {4, 4, 1, true}},
        {"int",   {4, 4, 1, false}},
        {"uint",  {4, 4, 1, false}},
        {"vec2",  {8, 8, 2, true}},
        {"vec3",  {16, 12, 3, true}},
        {"vec4",  {16, 16, 4, true}},
        {"ivec2", {8, 8, 2, false}},
        {"ivec3", {16, 12, 3, false}},
        {"ivec4", {16, 16, 4, false}},
        {"uvec2", {8, 8, 2, false}},
        {"uvec3", {16, 12, 3, false}},
        {"uvec4", {16, 16, 4, false}},
        {"mat4",  {16, 64, 16, true}},
    };
    for (const auto& e : kTypes) {
        const size_t n = std::strlen(e.name);
        if (n == t.size() && std::memcmp(e.name, t.data(), n) == 0) {
            out = e.info;
            return true;
        }
    }
    return false;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

string_view trim(string_view s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s.data()[b])) ++b;
    while (e > b && is_space(s.data()[e - 1])) --e;
    return s.substr(b, e - b);
}

string to_string(string_view sv) { return string(sv.data(), sv.size()); }

std::string key_of(string_view sv) { return std::string(sv.data(), sv.size()); }

/// True when a declaration chunk carries a preprocessor directive. Directives
/// have no terminating ';', so they fold into the following chunk; layout under
/// a conditional cannot be decided from the text alone, so the caller reports
/// this specifically rather than as a malformed member.
bool has_directive(string_view decl)
{
    for (size_t i = 0; i < decl.size(); ++i) {
        if (decl.data()[i] == '#') return true;
    }
    return false;
}

/// Splits "vec4 factor" into a type and its declarator names, supporting the
/// multi-declarator form "uint _pad0, _pad1". Returns false for anything else
/// (arrays, qualifiers, malformed text), which the caller turns into a hard
/// failure rather than a dropped field.
bool split_decl(string_view decl, string_view& type, vector<string_view>& names)
{
    names.clear();
    decl = trim(decl);
    if (decl.size() == 0) return false;
    for (size_t i = 0; i < decl.size(); ++i) {
        const char c = decl.data()[i];
        if (c == '[' || c == ']') return false;  // arrays are not modelled
    }
    size_t sp = decl.size();
    for (size_t i = 0; i < decl.size(); ++i) {
        if (is_space(decl.data()[i])) { sp = i; break; }
    }
    if (sp == decl.size()) return false;
    type = trim(decl.substr(0, sp));
    if (type.size() == 0) return false;

    string_view rest = trim(decl.substr(sp));
    for (;;) {
        const size_t comma = rest.find(',');
        string_view one = trim(comma == string_view::npos ? rest : rest.substr(0, comma));
        if (one.size() == 0) return false;
        for (size_t i = 0; i < one.size(); ++i) {
            if (is_space(one.data()[i])) return false;  // extra token / qualifier
        }
        names.push_back(one);
        if (comma == string_view::npos) break;
        rest = trim(rest.substr(comma + 1));
    }
    return !names.empty();
}

/// Blanks out `//` and slash-star comments, replacing their characters with
/// spaces so every index in the result still matches the input. Callers can
/// therefore locate a token in the stripped copy and slice the original at the
/// same offsets. Without this, prose containing the word "struct" (or a
/// commented-out member) is parsed as declaration text.
string strip_comments(string_view src)
{
    string out(src.data(), src.size());
    const size_t n = out.size();
    for (size_t i = 0; i < n;) {
        if (out[i] == '/' && i + 1 < n && out[i + 1] == '/') {
            while (i < n && out[i] != '\n') out[i++] = ' ';
            continue;
        }
        if (out[i] == '/' && i + 1 < n && out[i + 1] == '*') {
            out[i++] = ' ';
            out[i++] = ' ';
            while (i < n) {
                const bool end = (out[i] == '*' && i + 1 < n && out[i + 1] == '/');
                if (out[i] != '\n') out[i] = ' ';
                ++i;
                if (end) {
                    if (i < n) out[i++] = ' ';
                    break;
                }
            }
            continue;
        }
        ++i;
    }
    return out;
}

using StructMap = std::unordered_map<std::string, string_view>;

/// Collects `struct Name { body };` declarations, mapping name to body text.
void collect_structs(string_view src, StructMap& out)
{
    const string_view kw("struct");
    size_t pos = 0;
    for (;;) {
        const size_t s = src.find(kw, pos);
        if (s == string_view::npos) break;
        const size_t after = s + kw.size();
        if (after >= src.size() || !is_space(src.data()[after])) { pos = after; continue; }
        const size_t open = src.find('{', after);
        if (open == string_view::npos) break;
        const size_t close = src.find('}', open);
        if (close == string_view::npos) break;
        const string_view name = trim(src.substr(after, open - after));
        if (name.size() > 0) {
            out[key_of(name)] = src.substr(open + 1, close - open - 1);
        }
        pos = close + 1;
    }
}

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

/// std430 alignment of a struct: the largest alignment among its members,
/// recursively. Returns 0 when the struct or one of its member types is
/// unknown, which the caller reports as an unsupported layout.
uint32_t struct_align(const StructMap& structs, string_view name, uint32_t depth)
{
    if (depth > 8) return 0;
    auto it = structs.find(key_of(name));
    if (it == structs.end()) return 0;
    uint32_t best = 4;
    string_view body = it->second;
    vector<string_view> members;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t semi = body.find(';', pos);
        if (semi == string_view::npos) break;
        const string_view decl = body.substr(pos, semi - pos);
        pos = semi + 1;
        if (trim(decl).size() == 0) continue;
        if (has_directive(decl)) return 0;
        string_view type;
        if (!split_decl(decl, type, members)) return 0;
        ScalarType info;
        if (leaf_type(type, info)) {
            if (info.align > best) best = info.align;
            continue;
        }
        const uint32_t nested = struct_align(structs, type, depth + 1);
        if (nested == 0) return 0;
        if (nested > best) best = nested;
    }
    return best;
}

} // namespace

string_view find_material_type(string_view glsl_src)
{
    // Searched in a comment-stripped copy so a commented-out declaration is
    // not picked up. Stripping preserves indices, so the match offsets slice
    // the caller's original buffer and the returned view stays valid.
    const string stripped = strip_comments(glsl_src);
    const string_view src(stripped);

    const string_view kMarker("VELK_MATERIAL(");
    size_t open = src.find(kMarker);
    if (open == string_view::npos) return {};
    open += kMarker.size();
    size_t end = open;
    while (end < src.size()) {
        const char c = src.data()[end];
        if (c == ',' || c == ')') break;
        ++end;
    }
    if (end >= src.size()) return {};
    return trim(glsl_src.substr(open, end - open));
}

MaterialLayout parse_material_layout(string_view glsl_src, string_view type_name)
{
    MaterialLayout layout;
    // Parsed from a comment-stripped copy, kept alive for the whole call: the
    // struct map and every intermediate view point into it. Field names and
    // types are copied out, so nothing escapes.
    const string stripped = strip_comments(glsl_src);
    StructMap structs;
    collect_structs(string_view(stripped), structs);

    // Lays out one struct at absolute byte offset @p base, appending its
    // non-padding leaves. @p cursor tracks the offset within this struct.
    auto walk = [&](auto&& self, string_view name, uint32_t base,
                    const string& prefix, uint32_t depth, uint32_t& cursor) -> bool {
        if (depth > 8) {
            layout.error = string("material struct nesting too deep");
            return false;
        }
        auto it = structs.find(key_of(name));
        if (it == structs.end()) {
            layout.error = string("no declaration found for struct ") + to_string(name);
            return false;
        }
        const string_view body = it->second;

        vector<string_view> members;
        size_t pos = 0;
        while (pos < body.size()) {
            const size_t semi = body.find(';', pos);
            if (semi == string_view::npos) break;
            const string_view decl = body.substr(pos, semi - pos);
            pos = semi + 1;
            if (trim(decl).size() == 0) continue;

            if (has_directive(decl)) {
                layout.error = string("preprocessor directive inside struct ")
                             + to_string(name)
                             + string(": member layout cannot be derived from conditional text");
                return false;
            }

            string_view type;
            if (!split_decl(decl, type, members)) {
                layout.error = string("unsupported member declaration \"")
                             + to_string(trim(decl)) + string("\" in ") + to_string(name)
                             + string(" (arrays and qualifiers are not supported)");
                return false;
            }

            ScalarType info;
            const bool is_leaf = leaf_type(type, info);
            uint32_t nested_align = 0;
            if (!is_leaf) {
                nested_align = struct_align(structs, type, depth + 1);
                if (nested_align == 0) {
                    layout.error = string("unsupported member type \"") + to_string(type)
                                 + string("\" in ") + to_string(name);
                    return false;
                }
            }

            // Each declarator of a multi-declarator gets its own aligned slot.
            for (const auto& member : members) {
                if (is_leaf) {
                    cursor = align_up(cursor, info.align);
                    if (member.data()[0] != '_') {  // '_' prefix marks padding
                        MaterialField f;
                        f.name = prefix + to_string(member);
                        f.type = to_string(type);
                        f.offset = base + cursor;
                        f.words = info.words;
                        layout.fields.push_back(std::move(f));
                    }
                    cursor += info.size;
                    continue;
                }
                cursor = align_up(cursor, nested_align);
                uint32_t nested_cursor = 0;
                if (!self(self, type, base + cursor,
                          prefix + to_string(member) + string("."), depth + 1,
                          nested_cursor)) {
                    return false;
                }
                cursor += align_up(nested_cursor, nested_align);
            }
        }
        return true;
    };

    uint32_t cursor = 0;
    if (!walk(walk, type_name, 0, string(), 0, cursor)) {
        layout.fields.clear();
        return layout;
    }
    const uint32_t root_align = struct_align(structs, type_name, 0);
    layout.size = align_up(cursor, root_align ? root_align : 4);
    layout.ok = true;
    return layout;
}

string validate_material_layout(const MaterialLayout& parsed,
                                const vector<ShaderParam>& reflected)
{
    char buf[128];
    for (const auto& r : reflected) {
        const MaterialField* match = nullptr;
        for (const auto& f : parsed.fields) {
            if (f.name.size() == r.name.size()
                && std::memcmp(f.name.c_str(), r.name.c_str(), f.name.size()) == 0) {
                match = &f;
                break;
            }
        }
        if (!match) {
            return string("reflected field \"") + r.name
                 + string("\" is missing from the parsed layout");
        }
        if (match->offset != r.offset) {
            const int n = std::snprintf(buf, sizeof(buf),
                                        "\": parsed offset %u, compiler says %u",
                                        match->offset, r.offset);
            return string("layout mismatch for \"") + r.name
                 + string(string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
        }
    }
    return string();
}

string generate_material_unpacker(string_view fn_name, string_view type_name,
                                  const MaterialLayout& layout)
{
    string out;
    char buf[192];

    out += type_name;
    out += string_view(" ");
    out += fn_name;
    out += string_view("(uint b) {\n    ");
    out += type_name;
    out += string_view(" d;\n");

    for (const auto& f : layout.fields) {
        ScalarType info;
        if (!leaf_type(string_view(f.type), info)) continue;
        const uint32_t w0 = f.offset / 4u;

        out += string_view("    d.");
        out += f.name;
        out += string_view(" = ");
        if (info.words > 1) {
            out += f.type;
            out += string_view("(");
        }
        for (uint32_t i = 0; i < info.words; ++i) {
            const int n = std::snprintf(
                buf, sizeof(buf),
                info.is_float ? "%suintBitsToFloat(velk_material_words.data[b + %u])"
                              : "%svelk_material_words.data[b + %u]",
                i ? ", " : "", w0 + i);
            out += string_view(buf, n > 0 ? static_cast<size_t>(n) : 0);
        }
        if (info.words > 1) {
            out += string_view(")");
        }
        out += string_view(";\n");
    }

    out += string_view("    return d;\n}\n");
    return out;
}

} // namespace velk
