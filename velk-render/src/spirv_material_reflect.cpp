#include "spirv_material_reflect.h"

#include <velk/api/math_types.h>
#include <velk/common.h>

#include <cstring>
#include <string>
#include <unordered_map>

namespace velk {

namespace {

// SPIR-V opcodes we care about
constexpr uint32_t SpvOpName = 5;
constexpr uint32_t SpvOpMemberName = 6;
constexpr uint32_t SpvOpTypeVoid = 19;
constexpr uint32_t SpvOpTypeBool = 20;
constexpr uint32_t SpvOpTypeInt = 21;
constexpr uint32_t SpvOpTypeFloat = 22;
constexpr uint32_t SpvOpTypeVector = 23;
constexpr uint32_t SpvOpTypeMatrix = 24;
constexpr uint32_t SpvOpTypeStruct = 30;
constexpr uint32_t SpvOpTypePointer = 32;
constexpr uint32_t SpvOpMemberDecorate = 72;

// SPIR-V decoration
constexpr uint32_t SpvDecorationOffset = 35;

// The standard DrawDataHeader has these fields (32 bytes total),
// matching the VELK_DRAW_DATA macro expansion in shader_compiler.cpp:
//   globals (buffer_reference, 8 bytes)
//   instances (buffer_reference, 8 bytes)
//   texture_id (uint, 4 bytes)
//   instance_count (uint, 4 bytes)
//   vbo (buffer_reference, 8 bytes)
// After the header a single `material` buffer_reference slot points at
// the per-material params struct; reflection descends into that struct
// to enumerate the user-facing inputs.
constexpr uint32_t kHeaderFieldCount = 5;
constexpr uint32_t kHeaderSize = 32;

struct TypeInfo
{
    enum Kind
    {
        Unknown,
        Float,
        Int,
        UInt,
        Vec2,
        Vec3,
        Vec4,
        Mat4
    };
    Kind kind = Unknown;
    uint32_t size = 0;
};

Uid type_info_to_uid(TypeInfo::Kind kind)
{
    switch (kind) {
    case TypeInfo::Float:
        return type_uid<float>();
    case TypeInfo::Int:
        return type_uid<int32_t>();
    case TypeInfo::UInt:
        return type_uid<uint32_t>();
    case TypeInfo::Vec2:
        return type_uid<vec2>();
    case TypeInfo::Vec3:
        return type_uid<vec3>();
    case TypeInfo::Vec4:
        return type_uid<color>();
    case TypeInfo::Mat4:
        return type_uid<mat4>();
    default:
        return {};
    }
}

uint32_t type_info_size(TypeInfo::Kind kind)
{
    switch (kind) {
    case TypeInfo::Float:
        return 4;
    case TypeInfo::Int:
        return 4;
    case TypeInfo::UInt:
        return 4;
    case TypeInfo::Vec2:
        return 8;
    case TypeInfo::Vec3:
        return 12;
    case TypeInfo::Vec4:
        return 16;
    case TypeInfo::Mat4:
        return 64;
    default:
        return 0;
    }
}

const char* read_spirv_string(const uint32_t* words, size_t max_words)
{
    return reinterpret_cast<const char*>(words);
}

} // namespace

vector<ShaderParam> reflect_material_params(const uint32_t* spirv, size_t word_count)
{
    if (!spirv || word_count < 5) {
        return {};
    }

    // Verify SPIR-V magic
    if (spirv[0] != 0x07230203) {
        return {};
    }

    // First pass: collect names, types, member decorations
    std::unordered_map<uint32_t, string> id_names;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, string>>
        member_names; // id -> (member_idx -> name)
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>
        member_offsets; // id -> (member_idx -> offset)
    std::unordered_map<uint32_t, TypeInfo> type_infos;
    std::unordered_map<uint32_t, std::vector<uint32_t>> struct_members; // struct_id -> member type ids
    std::unordered_map<uint32_t, uint32_t> pointer_targets;           // pointer_id -> pointee_id

    size_t pos = 5; // skip header
    while (pos < word_count) {
        uint32_t instruction = spirv[pos];
        uint32_t opcode = instruction & 0xFFFF;
        uint32_t length = instruction >> 16;

        if (length == 0 || pos + length > word_count) {
            break;
        }

        switch (opcode) {
        case SpvOpName: {
            if (length >= 3) {
                uint32_t id = spirv[pos + 1];
                id_names[id] = read_spirv_string(&spirv[pos + 2], length - 2);
            }
            break;
        }
        case SpvOpMemberName: {
            if (length >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t member = spirv[pos + 2];
                member_names[id][member] = read_spirv_string(&spirv[pos + 3], length - 3);
            }
            break;
        }
        case SpvOpMemberDecorate: {
            if (length >= 5) {
                uint32_t id = spirv[pos + 1];
                uint32_t member = spirv[pos + 2];
                uint32_t decoration = spirv[pos + 3];
                if (decoration == SpvDecorationOffset) {
                    member_offsets[id][member] = spirv[pos + 4];
                }
            }
            break;
        }
        case SpvOpTypeFloat: {
            if (length >= 3) {
                uint32_t id = spirv[pos + 1];
                uint32_t width = spirv[pos + 2];
                if (width == 32) {
                    type_infos[id] = {TypeInfo::Float, 4};
                }
            }
            break;
        }
        case SpvOpTypeInt: {
            if (length >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t width = spirv[pos + 2];
                uint32_t signedness = spirv[pos + 3];
                if (width == 32) {
                    type_infos[id] = {signedness ? TypeInfo::Int : TypeInfo::UInt, 4};
                }
            }
            break;
        }
        case SpvOpTypeVector: {
            if (length >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t component_id = spirv[pos + 2];
                uint32_t count = spirv[pos + 3];
                auto cit = type_infos.find(component_id);
                if (cit != type_infos.end() && cit->second.kind == TypeInfo::Float) {
                    TypeInfo::Kind kind = TypeInfo::Unknown;
                    if (count == 2) {
                        kind = TypeInfo::Vec2;
                    } else if (count == 3) {
                        kind = TypeInfo::Vec3;
                    } else if (count == 4) {
                        kind = TypeInfo::Vec4;
                    }
                    if (kind != TypeInfo::Unknown) {
                        type_infos[id] = {kind, type_info_size(kind)};
                    }
                }
            }
            break;
        }
        case SpvOpTypeMatrix: {
            if (length >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t column_type = spirv[pos + 2];
                uint32_t column_count = spirv[pos + 3];
                auto cit = type_infos.find(column_type);
                if (cit != type_infos.end() && cit->second.kind == TypeInfo::Vec4 && column_count == 4) {
                    type_infos[id] = {TypeInfo::Mat4, 64};
                }
            }
            break;
        }
        case SpvOpTypeStruct: {
            if (length >= 2) {
                uint32_t id = spirv[pos + 1];
                std::vector<uint32_t> members;
                for (uint32_t i = 2; i < length; ++i) {
                    members.push_back(spirv[pos + i]);
                }
                struct_members[id] = std::move(members);
            }
            break;
        }
        case SpvOpTypePointer: {
            // id, storage_class, pointee_type_id
            if (length >= 4) {
                uint32_t id = spirv[pos + 1];
                uint32_t pointee = spirv[pos + 3];
                pointer_targets[id] = pointee;
            }
            break;
        }
        default:
            break;
        }

        pos += length;
    }

    // Find the struct named "DrawData"
    uint32_t draw_data_id = 0;
    for (auto& [id, name] : id_names) {
        if (name == "DrawData") {
            draw_data_id = id;
            break;
        }
    }

    if (draw_data_id == 0) {
        return {};
    }

    auto sit = struct_members.find(draw_data_id);
    if (sit == struct_members.end()) {
        return {};
    }

    auto& dd_members = sit->second;
    auto& dd_names = member_names[draw_data_id];

    // After the header there must be a single buffer_reference pointer
    // field (conventionally named `material`) that points at the
    // per-material params struct. Find it, follow the pointer, and
    // enumerate that struct's members as the shader inputs.
    uint32_t params_struct_id = 0;
    for (uint32_t i = kHeaderFieldCount; i < static_cast<uint32_t>(dd_members.size()); ++i) {
        auto nit = dd_names.find(i);
        if (nit == dd_names.end()) continue;
        const string& name = nit->second;
        if (!name.empty() && name[0] == '_') continue; // padding
        auto pit = pointer_targets.find(dd_members[i]);
        if (pit == pointer_targets.end()) continue;
        if (struct_members.find(pit->second) == struct_members.end()) continue;
        params_struct_id = pit->second;
        break;
    }

    vector<ShaderParam> params;
    if (params_struct_id == 0) {
        return params;
    }

    auto& mat_members = struct_members[params_struct_id];
    auto& mat_names = member_names[params_struct_id];
    auto& mat_offsets = member_offsets[params_struct_id];

    for (uint32_t i = 0; i < static_cast<uint32_t>(mat_members.size()); ++i) {
        auto nit = mat_names.find(i);
        if (nit == mat_names.end()) {
            continue;
        }

        const string& name = nit->second;
        if (!name.empty() && name[0] == '_') {
            continue; // padding
        }

        auto oit = mat_offsets.find(i);
        if (oit == mat_offsets.end()) {
            continue;
        }

        uint32_t member_type_id = mat_members[i];
        auto tit = type_infos.find(member_type_id);
        if (tit == type_infos.end()) {
            continue;
        }

        auto& ti = tit->second;
        Uid uid = type_info_to_uid(ti.kind);
        if (uid == Uid{}) {
            continue;
        }

        ShaderParam param;
        param.name = string(name.c_str(), name.size());
        param.type_uid = uid;
        param.offset = oit->second;
        param.size = type_info_size(ti.kind);
        params.push_back(std::move(param));
    }

    return params;
}

} // namespace velk
