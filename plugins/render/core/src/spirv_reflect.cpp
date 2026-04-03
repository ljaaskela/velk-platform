#include "spirv_reflect.h"

#include <velk/api/math_types.h>

#include <cstring>
#include <unordered_map>

namespace velk_ui {

namespace {

// SPIR-V opcodes
constexpr uint32_t SpvOpName = 5;
constexpr uint32_t SpvOpMemberName = 6;
constexpr uint32_t SpvOpDecorate = 71;
constexpr uint32_t SpvOpMemberDecorate = 72;
constexpr uint32_t SpvOpTypeFloat = 22;
constexpr uint32_t SpvOpTypeVector = 23;
constexpr uint32_t SpvOpTypeMatrix = 24;
constexpr uint32_t SpvOpTypeImage = 25;
constexpr uint32_t SpvOpTypeSampledImage = 27;
constexpr uint32_t SpvOpTypeStruct = 30;
constexpr uint32_t SpvOpTypePointer = 32;
constexpr uint32_t SpvOpVariable = 59;

// Decorations
constexpr uint32_t SpvDecorationBinding = 33;
constexpr uint32_t SpvDecorationDescriptorSet = 34;
constexpr uint32_t SpvDecorationOffset = 35;
constexpr uint32_t SpvDecorationBlock = 2;

// Storage classes
constexpr uint32_t SpvStorageClassUniformConstant = 0;
constexpr uint32_t SpvStorageClassUniform = 2;

velk::Uid spirv_type_to_velk_uid(uint32_t type_id,
                                  const std::unordered_map<uint32_t, uint32_t>& type_opcodes,
                                  const std::unordered_map<uint32_t, uint32_t>& vector_components)
{
    auto oit = type_opcodes.find(type_id);
    if (oit == type_opcodes.end()) return {};

    uint32_t opcode = oit->second;

    if (opcode == SpvOpTypeSampledImage || opcode == SpvOpTypeImage)
        return velk::type_uid<int32_t>();
    if (opcode == SpvOpTypeFloat)
        return velk::type_uid<float>();
    if (opcode == SpvOpTypeMatrix)
        return velk::type_uid<velk::mat4>();
    if (opcode == SpvOpTypeVector) {
        auto cit = vector_components.find(type_id);
        uint32_t count = (cit != vector_components.end()) ? cit->second : 0;
        if (count == 2) return velk::type_uid<velk::vec2>();
        if (count == 4) return velk::type_uid<velk::color>();
    }
    return {};
}

} // namespace

velk::vector<UniformInfo> reflect_spirv_uniforms(const uint32_t* spirv, size_t word_count)
{
    if (!spirv || word_count < 5) return {};

    std::unordered_map<uint32_t, velk::string> names;              // id -> name
    std::unordered_map<uint32_t, int> bindings;                     // id -> binding
    std::unordered_map<uint32_t, uint32_t> pointer_to_pointee;     // ptr type -> pointee type
    std::unordered_map<uint32_t, uint32_t> type_opcodes;           // type id -> opcode
    std::unordered_map<uint32_t, uint32_t> vector_components;      // vec type -> count
    std::unordered_map<uint32_t, bool> block_decorated;            // struct id -> has Block decoration

    // struct_id -> { member_index -> name }
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, velk::string>> member_names;
    // struct_id -> { member_index -> offset }
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> member_offsets;
    // struct_id -> { member_index -> type_id }
    std::unordered_map<uint32_t, velk::vector<uint32_t>> struct_member_types;

    struct VarInfo {
        uint32_t id;
        uint32_t type_id;
        uint32_t storage_class;
    };
    velk::vector<VarInfo> variables;

    size_t pos = 5;
    while (pos < word_count) {
        uint32_t word = spirv[pos];
        uint32_t opcode = word & 0xFFFF;
        uint32_t len = word >> 16;
        if (len == 0) break;

        switch (opcode) {
        case SpvOpName:
            if (len >= 3) {
                names[spirv[pos + 1]] = velk::string(reinterpret_cast<const char*>(&spirv[pos + 2]));
            }
            break;

        case SpvOpMemberName:
            if (len >= 4) {
                uint32_t struct_id = spirv[pos + 1];
                uint32_t member = spirv[pos + 2];
                const char* str = reinterpret_cast<const char*>(&spirv[pos + 3]);
                member_names[struct_id][member] = velk::string(str);
            }
            break;

        case SpvOpDecorate:
            if (len >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t decoration = spirv[pos + 2];
                if (decoration == SpvDecorationBinding) bindings[id] = static_cast<int>(spirv[pos + 3]);
                if (decoration == SpvDecorationBlock) block_decorated[id] = true;
            } else if (len >= 3) {
                uint32_t id = spirv[pos + 1];
                uint32_t decoration = spirv[pos + 2];
                if (decoration == SpvDecorationBlock) block_decorated[id] = true;
            }
            break;

        case SpvOpMemberDecorate:
            if (len >= 5) {
                uint32_t struct_id = spirv[pos + 1];
                uint32_t member = spirv[pos + 2];
                uint32_t decoration = spirv[pos + 3];
                uint32_t value = spirv[pos + 4];
                if (decoration == SpvDecorationOffset) member_offsets[struct_id][member] = value;
            }
            break;

        case SpvOpTypeFloat:
            if (len >= 2) type_opcodes[spirv[pos + 1]] = opcode;
            break;

        case SpvOpTypeVector:
            if (len >= 4) {
                type_opcodes[spirv[pos + 1]] = opcode;
                vector_components[spirv[pos + 1]] = spirv[pos + 3];
            }
            break;

        case SpvOpTypeMatrix:
            if (len >= 4) type_opcodes[spirv[pos + 1]] = opcode;
            break;

        case SpvOpTypeImage:
            if (len >= 2) type_opcodes[spirv[pos + 1]] = opcode;
            break;

        case SpvOpTypeSampledImage:
            if (len >= 2) type_opcodes[spirv[pos + 1]] = opcode;
            break;

        case SpvOpTypeStruct:
            if (len >= 2) {
                uint32_t id = spirv[pos + 1];
                type_opcodes[id] = opcode;
                velk::vector<uint32_t> members;
                for (uint32_t i = 2; i < len; ++i) {
                    members.push_back(spirv[pos + i]);
                }
                struct_member_types[id] = std::move(members);
            }
            break;

        case SpvOpTypePointer:
            if (len >= 4) {
                pointer_to_pointee[spirv[pos + 1]] = spirv[pos + 3];
            }
            break;

        case SpvOpVariable:
            if (len >= 4) {
                VarInfo v;
                v.type_id = spirv[pos + 1];
                v.id = spirv[pos + 2];
                v.storage_class = spirv[pos + 3];
                variables.push_back(v);
            }
            break;
        }

        pos += len;
    }

    velk::vector<UniformInfo> result;

    for (auto& v : variables) {
        // Follow pointer to get the actual type
        uint32_t pointee_type = v.type_id;
        auto pit = pointer_to_pointee.find(pointee_type);
        if (pit != pointer_to_pointee.end()) pointee_type = pit->second;

        // Check if this is a UBO (Uniform storage class + Block-decorated struct)
        if (v.storage_class == SpvStorageClassUniform && block_decorated.count(pointee_type)) {
            // Get the block name to skip VelkGlobals (handled by backend directly)
            auto nit = names.find(pointee_type);
            velk::string block_name = (nit != names.end()) ? nit->second : velk::string{};
            if (block_name == "VelkGlobals" || block_name == "PushConstants") continue;

            // Reflect UBO members as uniforms with byte offsets
            auto mit = struct_member_types.find(pointee_type);
            if (mit == struct_member_types.end()) continue;

            auto& members = mit->second;
            for (uint32_t i = 0; i < static_cast<uint32_t>(members.size()); ++i) {
                UniformInfo info;

                auto mn = member_names.find(pointee_type);
                if (mn != member_names.end()) {
                    auto mnit = mn->second.find(i);
                    if (mnit != mn->second.end()) info.name = mnit->second;
                }

                auto mo = member_offsets.find(pointee_type);
                if (mo != member_offsets.end()) {
                    auto moit = mo->second.find(i);
                    info.location = (moit != mo->second.end()) ? static_cast<int>(moit->second) : -1;
                }

                info.typeUid = spirv_type_to_velk_uid(members[i], type_opcodes, vector_components);

                if (info.location >= 0 && !info.name.empty()) {
                    result.push_back(std::move(info));
                }
            }
            continue;
        }

        // Opaque uniforms (samplers) remain as before
        if (v.storage_class == SpvStorageClassUniformConstant) {
            UniformInfo info;
            auto nit = names.find(v.id);
            if (nit != names.end()) info.name = nit->second;

            auto bit = bindings.find(v.id);
            info.location = (bit != bindings.end()) ? bit->second : -1;
            info.typeUid = spirv_type_to_velk_uid(pointee_type, type_opcodes, vector_components);

            if (info.location >= 0) {
                result.push_back(std::move(info));
            }
        }
    }

    return result;
}

} // namespace velk_ui
