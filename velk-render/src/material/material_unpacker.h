#ifndef VELK_RENDER_MATERIAL_UNPACKER_H
#define VELK_RENDER_MATERIAL_UNPACKER_H

#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk {

struct ShaderParam;

/// One leaf field of a material record, flattened out of any struct nesting.
struct MaterialField
{
    string name;      ///< Dotted path from the record root, e.g. "base_color.factor".
    string type;      ///< GLSL leaf type: float / int / uint / vec2..4 / ivec2..4 / uvec2..4 / mat3 / mat4.
    uint32_t offset;  ///< Byte offset within the record (std430).
    uint32_t words;   ///< 32-bit words the value itself occupies (excludes trailing padding).
};

/// Flattened std430 layout of a material record, or the reason it could not
/// be derived. Padding members (names starting with '_') are accounted for in
/// the offsets but are not reported as fields.
struct MaterialLayout
{
    vector<MaterialField> fields;
    uint32_t size = 0;  ///< std430 size of the whole record, including tail padding.
    bool ok = false;
    string error;  ///< Human-readable reason when @c ok is false.
};

/**
 * @brief Derives the std430 layout of a material record by parsing the GLSL
 *        struct declarations in a material snippet.
 *
 * Collects every `struct Name { ... };` in @p glsl_src, then walks
 * @p type_name, descending into members whose type is one of those structs and
 * folding their offsets and names together.
 *
 * This deliberately mirrors what the GLSL compiler does for std430 rather than
 * what the C++ mirror struct does, because the raster path binds this record
 * as a typed std430 block. `validate_material_layout` cross-checks the result
 * against SPIR-V reflection of that block, which is the authority.
 *
 * Comments are stripped first, and the multi-declarator form
 * (`uint _pad0, _pad1;`) is supported. Unsupported constructs (arrays,
 * qualifiers, unknown types, unresolved struct members, and preprocessor
 * directives inside a struct body, whose layout depends on which branch the
 * compiler takes) produce `ok = false` with an explanatory `error` rather than
 * a partial layout, so a material can never silently lose a field.
 */
MaterialLayout parse_material_layout(string_view glsl_src, string_view type_name);

/**
 * @brief Finds the material record's type name in a snippet.
 *
 * Reads the argument of the snippet's `VELK_MATERIAL(T)` declaration, which
 * every material places between its struct declarations and its eval function.
 *
 * @return The type name, or an empty view when the snippet declares no
 *         material record (materials that take no parameters).
 */
string_view find_material_type(string_view glsl_src);

/**
 * @brief Cross-checks a parsed layout against SPIR-V reflection of the same
 *        record.
 *
 * Reflection offsets come from the compiler's own `Offset` decorations, so any
 * disagreement means the parser's std430 rules are wrong for this record.
 *
 * @return Empty on agreement; otherwise a description of the first mismatch,
 *         suitable for a hard failure at material registration.
 */
string validate_material_layout(const MaterialLayout& parsed,
                                const vector<ShaderParam>& reflected);

/**
 * @brief Emits a GLSL function that rebuilds @p type_name from a bound
 *        `uint` array, for paths that cannot bind the record as a typed block.
 *
 * The generated function has the form
 * `TypeName fn_name(uint b) { ... }`, reading `velk_material_words.data[b + n]`
 * and reinterpreting via `uintBitsToFloat` where the field is floating point.
 * @p b is a word index, so callers pass `byte_offset / 4`.
 */
string generate_material_unpacker(string_view fn_name, string_view type_name,
                                  const MaterialLayout& layout);

} // namespace velk

#endif // VELK_RENDER_MATERIAL_UNPACKER_H
