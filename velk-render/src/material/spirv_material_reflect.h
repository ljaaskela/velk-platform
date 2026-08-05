#ifndef VELK_RENDER_SPIRV_MATERIAL_REFLECT_H
#define VELK_RENDER_SPIRV_MATERIAL_REFLECT_H

#include <velk/string.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk {

/// Describes a single material parameter discovered in a shader's material struct.
struct ShaderParam
{
    /// Member name. Members of nested structs are dotted paths from the
    /// record root, e.g. "base_color.factor".
    string name;
    Uid type_uid;    ///< velk type UID (e.g. type_uid<float>(), type_uid<color>())
    uint32_t offset; ///< Byte offset within the material record (relative to the struct start)
    uint32_t size;   ///< Size in bytes
};

/**
 * @brief Reflects material parameters from compiled SPIR-V bytecode.
 *
 * Looks for the set = 1 material block declared by VELK_MATERIAL(T) (a
 * `buffer VelkMaterials { T data[]; }`), follows its runtime array to the
 * element struct T, and enumerates T's members as the material inputs.
 *
 * Nested structs are flattened: their members are reported with the parent's
 * byte offset folded in and its name prefixed, so a record that groups fields
 * into sub-structs (as StandardMaterialData does) yields the same flat list a
 * record with those fields inline would.
 *
 * Fields starting with '_' are treated as padding and skipped.
 *
 * @param spirv  SPIR-V bytecode (typically the fragment shader, where the
 *        material record is read).
 * @param word_count  Number of 32-bit words in the bytecode.
 * @return Material parameters. Empty if no material block found.
 */
vector<ShaderParam> reflect_material_params(const uint32_t* spirv, size_t word_count);

} // namespace velk

#endif // VELK_RENDER_SPIRV_MATERIAL_REFLECT_H
