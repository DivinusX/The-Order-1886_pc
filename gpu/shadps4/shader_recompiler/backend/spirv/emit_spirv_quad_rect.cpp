// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/backend/spirv/bary_emulation.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

constexpr u32 SPIRV_VERSION_1_5 = 0x00010500;

struct QuadRectListEmitter : public Sirit::Module {
    explicit QuadRectListEmitter(const HwFragmentRuntimeInfo& fs_info_)
        : Sirit::Module{SPIRV_VERSION_1_5}, fs_info{fs_info_} {
        void_id = TypeVoid();
        bool_id = TypeBool();
        float_id = TypeFloat(32);
        uint_id = TypeUInt(32U);
        int_id = TypeInt(32U, true);
        bvec2_id = TypeVector(bool_id, 2);
        vec2_id = TypeVector(float_id, 2);
        vec3_id = TypeVector(float_id, 3);
        vec4_id = TypeVector(float_id, 4);

        float_one = Constant(float_id, 1.0f);
        float_min_one = Constant(float_id, -1.0f);
        int_zero = Constant(int_id, 0);

        const Id float_arr{TypeArray(float_id, Constant(uint_id, 1U))};
        gl_per_vertex_type = TypeStruct(vec4_id, float_id, float_arr, float_arr);
        Decorate(gl_per_vertex_type, spv::Decoration::Block);
        MemberDecorate(gl_per_vertex_type, 0U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        MemberDecorate(gl_per_vertex_type, 1U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::PointSize));
        MemberDecorate(gl_per_vertex_type, 2U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::ClipDistance));
        MemberDecorate(gl_per_vertex_type, 3U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::CullDistance));
    }

    /// Emits tessellation control shader for interpolating the 4th vertex of rectange primitive
    void EmitRectListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);

        // Set passthrough tessellation factors
        const Id output_float_id{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};

        // Emit interpolation block of the 4th vertex in rect.
        // Load positions
        std::array<Id, 3> pos;
        for (int i = 0; i < 3; i++) {
            pos[i] = OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, Int(i), int_zero));
        }

        std::array<Id, 3> point_coord_equal;
        for (int i = 0; i < 3; i++) {
            // point_coord_equal[i] = equal(gl_in[i].gl_Position.xy, gl_in[(i + 1) %
            // 3].gl_Position.xy);
            const Id pos_l_xy{OpVectorShuffle(vec2_id, pos[i], pos[i], 0, 1)};
            const Id pos_r_xy{OpVectorShuffle(vec2_id, pos[(i + 1) % 3], pos[(i + 1) % 3], 0, 1)};
            point_coord_equal[i] = OpFOrdEqual(bvec2_id, pos_l_xy, pos_r_xy);
        }

        std::array<Id, 3> bary_coord;
        std::array<Id, 3> is_edge_vertex;
        for (int i = 0; i < 3; i++) {
            // bool xy_equal = point_coord_equal[i].x && point_coord_equal[(i + 2) % 3].y;
            const Id xy_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 0),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 1))};
            // bool yx_equal = point_coord_equal[i].y && point_coord_equal[(i + 2) % 3].x;
            const Id yx_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 1),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 0))};
            // bary_coord[i] = (xy_equal || yx_equal) ? -1.f : 1.f;
            is_edge_vertex[i] = OpLogicalOr(bool_id, xy_equal, yx_equal);
            bary_coord[i] = OpSelect(float_id, is_edge_vertex[i], float_min_one, float_one);
        }

        const auto interpolate = [&](Id v0, Id v1, Id v2) {
            // return v0 * bary_coord.x + v1 * bary_coord.y + v2 * bary_coord.z;
            const Id p0{OpVectorTimesScalar(vec4_id, v0, bary_coord[0])};
            const Id p1{OpVectorTimesScalar(vec4_id, v1, bary_coord[1])};
            const Id p2{OpVectorTimesScalar(vec4_id, v2, bary_coord[2])};
            return OpFAdd(vec4_id, p0, OpFAdd(vec4_id, p1, p2));
        };

        // int vertex_index_id = is_edge_vertex[1] ? 1 : (is_edge_vertex[2] ? 2 : 0);
        Id vertex_index{OpSelect(int_id, is_edge_vertex[2], Int(2), Int(0))};
        vertex_index = OpSelect(int_id, is_edge_vertex[1], Int(1), vertex_index);

        // int index = (vertex_index_id + gl_InvocationID) % 3;
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id invocation_3{OpIEqual(bool_id, invocation_id, Int(3))};
        const Id index{OpSMod(int_id, OpIAdd(int_id, vertex_index, invocation_id), Int(3))};

        // gl_out[gl_InvocationID].gl_Position = gl_InvocationID == 3 ? pos3 :
        // gl_in[index].gl_Position;
        const Id pos3{interpolate(pos[0], pos[1], pos[2])};
        const Id in_ptr{OpAccessChain(input_vec4, gl_in, index, Int(0))};
        const Id position{OpSelect(vec4_id, invocation_3, pos3, OpLoad(vec4_id, in_ptr))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), position);

        // Set attributes
        for (int i = 0; i < inputs.size(); i++) {
            // vec4 in_paramN3 = interpolate(bary_coord, in_paramN[0], in_paramN[1], in_paramN[2]);
            const Id v0{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(0)))};
            const Id v1{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(1)))};
            const Id v2{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(2)))};
            const Id in_param3{interpolate(v0, v1, v2)};
            // out_paramN[gl_InvocationID] = gl_InvocationID == 3 ? in_paramN3 : in_paramN[index];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            const Id out_param{OpSelect(vec4_id, invocation_3, in_param3, in_param)};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), out_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation control shader that outputs 4 control points.
    void EmitQuadListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);
        const Id array_type{TypeArray(int_id, Int(4))};
        const Id values{ConstantComposite(array_type, Int(1), Int(2), Int(0), Int(3))};
        const Id indices{AddLocalVariable(TypePointer(spv::StorageClass::Function, array_type),
                                          spv::StorageClass::Function, values)};

        // Set passthrough tessellation factors
        const Id output_float{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id func_int{TypePointer(spv::StorageClass::Function, int_id)};
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id index{OpLoad(int_id, OpAccessChain(func_int, indices, invocation_id))};

        // gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
        const Id in_position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), in_position);

        for (int i = 0; i < inputs.size(); i++) {
            // out_paramN[gl_InvocationID] = in_paramN[gl_InvocationID];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), in_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation evaluation shader that outputs 4 control points.
    void EmitPassthroughTES() {
        DefineEntry(spv::ExecutionModel::TessellationEvaluation);

        // const int index = int(gl_TessCoord.y) * 2 + int(gl_TessCoord.x);
        const Id input_float{TypePointer(spv::StorageClass::Input, float_id)};
        const Id tess_coord_x{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(0)))};
        const Id tess_coord_y{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(1)))};
        const Id index{OpIAdd(int_id, OpIMul(int_id, OpConvertFToS(int_id, tess_coord_y), Int(2)),
                              OpConvertFToS(int_id, tess_coord_x))};

        // gl_Position = gl_in[index].gl_Position;
        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_per_vertex, Int(0)), position);

        // out_paramN = in_paramN[index];
        for (int i = 0; i < inputs.size(); i++) {
            const Id param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(outputs[i], param);
        }

        OpReturn();
        OpFunctionEnd();
    }

private:
    Id Int(s32 value) {
        return Constant(int_id, value);
    }

    Id AddInput(Id type) {
        const Id input{AddGlobalVariable(TypePointer(spv::StorageClass::Input, type),
                                         spv::StorageClass::Input)};
        interfaces.push_back(input);
        return input;
    }

    Id AddOutput(Id type) {
        const Id output{AddGlobalVariable(TypePointer(spv::StorageClass::Output, type),
                                          spv::StorageClass::Output)};
        interfaces.push_back(output);
        return output;
    }

    void DefineEntry(spv::ExecutionModel model) {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Tessellation);
        const Id void_function{TypeFunction(void_id)};
        main = OpFunction(void_id, spv::FunctionControlMask::MaskNone, void_function);
        if (model == spv::ExecutionModel::TessellationControl) {
            AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 4U);
        } else {
            AddExecutionMode(main, spv::ExecutionMode::Quads);
            AddExecutionMode(main, spv::ExecutionMode::SpacingEqual);
            AddExecutionMode(main, spv::ExecutionMode::VertexOrderCw);
        }
        DefineInputs(model);
        DefineOutputs(model);
        AddEntryPoint(model, main, "main", interfaces);
        AddLabel(OpLabel());
    }

    void DefineOutputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationControl) {
            const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 4U))};
            gl_out = AddOutput(gl_per_vertex_array);

            const Id arr2_id{TypeArray(float_id, Constant(uint_id, 2U))};
            gl_tess_level_inner = AddOutput(arr2_id);
            Decorate(gl_tess_level_inner, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelInner);
            Decorate(gl_tess_level_inner, spv::Decoration::Patch);

            const Id arr4_id{TypeArray(float_id, Constant(uint_id, 4U))};
            gl_tess_level_outer = AddOutput(arr4_id);
            Decorate(gl_tess_level_outer, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelOuter);
            Decorate(gl_tess_level_outer, spv::Decoration::Patch);
        } else {
            gl_per_vertex = AddOutput(gl_per_vertex_type);
        }
        outputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            outputs.emplace_back(AddOutput(model == spv::ExecutionModel::TessellationControl
                                               ? TypeArray(vec4_id, Int(4))
                                               : vec4_id));
            Decorate(outputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

    void DefineInputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationEvaluation) {
            gl_tess_coord = AddInput(vec3_id);
            Decorate(gl_tess_coord, spv::Decoration::BuiltIn, spv::BuiltIn::TessCoord);
        } else {
            gl_invocation_id = AddInput(int_id);
            Decorate(gl_invocation_id, spv::Decoration::BuiltIn, spv::BuiltIn::InvocationId);
        }
        const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 32U))};
        gl_in = AddInput(gl_per_vertex_array);
        const Id float_arr{TypeArray(vec4_id, Int(32))};
        inputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            inputs.emplace_back(AddInput(float_arr));
            Decorate(inputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

private:
    HwFragmentRuntimeInfo fs_info;
    Id main;
    Id void_id;
    Id bool_id;
    Id float_id;
    Id uint_id;
    Id int_id;
    Id bvec2_id;
    Id vec2_id;
    Id vec3_id;
    Id vec4_id;
    Id float_one;
    Id float_min_one;
    Id int_zero;
    Id gl_per_vertex_type;
    Id gl_in;
    union {
        Id gl_out;
        Id gl_per_vertex;
    };
    Id gl_tess_level_inner;
    Id gl_tess_level_outer;
    union {
        Id gl_tess_coord;
        Id gl_invocation_id;
    };
    std::vector<Id> inputs;
    std::vector<Id> outputs;
    std::vector<Id> interfaces;
};

std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type, const HwFragmentRuntimeInfo& fs_info) {
    QuadRectListEmitter ctx{fs_info};
    switch (type) {
    case AuxShaderType::RectListTCS:
        ctx.EmitRectListTCS();
        break;
    case AuxShaderType::QuadListTCS:
        ctx.EmitQuadListTCS();
        break;
    case AuxShaderType::PassthroughTES:
        ctx.EmitPassthroughTES();
        break;
    }
    return ctx.Assemble();
}

std::vector<u32> EmitBarycentricGeometryShader(const HwFragmentRuntimeInfo& fs_rt,
                                               const Info& fs_info, const Info& vs_info,
                                               bool after_aux_tes) {
    if (!after_aux_tes && (vs_info.stores.GetAny(IR::Attribute::RenderTargetIndex) ||
                           vs_info.stores.GetAny(IR::Attribute::ViewportIndex))) {
        return {};
    }
    // The aux TES writes no clip-distance input for the fragment shader.
    if (after_aux_tes && fs_rt.clip_distance_emulation) {
        return {};
    }
    const auto layout = ComputeBaryEmulation(fs_rt, fs_info);
    if (!layout.needed || !layout.ok) {
        return {};
    }
    Sirit::Module m{SPIRV_VERSION_1_5};
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::Geometry);
    const Id void_id{m.TypeVoid()};
    const Id float_id{m.TypeFloat(32)};
    const Id uint_id{m.TypeUInt(32)};
    const Id int_id{m.TypeInt(32, true)};
    std::array<Id, 5> vec{};
    vec[1] = float_id;
    for (u32 n = 2; n <= 4; ++n) {
        vec[n] = m.TypeVector(float_id, n);
    }
    const auto u = [&](u32 v) { return m.Constant(uint_id, v); };
    const Id zero{m.Constant(float_id, 0.0f)};
    const Id one{m.Constant(float_id, 1.0f)};
    std::vector<Id> interfaces;
    const auto add = [&](Id type, spv::StorageClass storage) {
        const Id var{m.AddGlobalVariable(m.TypePointer(storage, type), storage)};
        interfaces.push_back(var);
        return var;
    };

    // gl_PerVertex with exactly the members the vertex shader writes: declaring ClipDistance it
    // never wrote would clip with garbage.
    // After the rect/quad-list aux TES the input block is that shader's fixed one (Position,
    // PointSize, ClipDistance[1], CullDistance[1], only Position written) and only Position is
    // passed on.
    const bool point_size = !after_aux_tes && vs_info.stores.GetAny(IR::Attribute::PointSize);
    const bool clip = !after_aux_tes && vs_info.stores.GetAny(IR::Attribute::ClipDistance) &&
                      !fs_rt.clip_distance_emulation;
    const bool cull = !after_aux_tes && vs_info.stores.GetAny(IR::Attribute::CullDistance);
    const Id dist_arr{m.TypeArray(float_id, u(8))};
    std::vector<Id> members{vec[4]};
    std::vector<spv::BuiltIn> builtins{spv::BuiltIn::Position};
    if (point_size) {
        members.push_back(float_id);
        builtins.push_back(spv::BuiltIn::PointSize);
    }
    if (clip) {
        members.push_back(dist_arr);
        builtins.push_back(spv::BuiltIn::ClipDistance);
    }
    if (cull) {
        members.push_back(dist_arr);
        builtins.push_back(spv::BuiltIn::CullDistance);
    }
    const auto per_vertex_type = [&](const std::vector<Id>& types,
                                     const std::vector<spv::BuiltIn>& names) {
        const Id type{m.TypeStruct(types)};
        m.Decorate(type, spv::Decoration::Block);
        for (u32 k = 0; k < names.size(); ++k) {
            m.MemberDecorate(type, k, spv::Decoration::BuiltIn, static_cast<u32>(names[k]));
        }
        return type;
    };
    const Id block{per_vertex_type(members, builtins)}; // the output block
    // Sirit merges identical types, so the aux-TES input block (a different member list) is
    // only built when needed; otherwise both directions share `block`.
    const Id in_block = [&] {
        if (!after_aux_tes) {
            return block;
        }
        const Id arr1{m.TypeArray(float_id, u(1))};
        return per_vertex_type({vec[4], float_id, arr1, arr1},
                               {spv::BuiltIn::Position, spv::BuiltIn::PointSize,
                                spv::BuiltIn::ClipDistance, spv::BuiltIn::CullDistance});
    }();
    const Id gl_in{add(m.TypeArray(in_block, u(3)), spv::StorageClass::Input)};
    const Id gl_out{add(block, spv::StorageClass::Output)};
    const Id prim_in{add(int_id, spv::StorageClass::Input)};
    m.Decorate(prim_in, spv::Decoration::BuiltIn, spv::BuiltIn::PrimitiveId);
    const Id prim_out{add(int_id, spv::StorageClass::Output)};
    m.Decorate(prim_out, spv::Decoration::BuiltIn, spv::BuiltIn::PrimitiveId);

    struct Attr {
        Id in, out;
        u32 width;
        bool corners;
    };
    std::vector<Attr> attrs;
    const auto attribute = [&](u32 source, u32 width, s32 corners) {
        const Id in{add(m.TypeArray(vec[width], u(3)), spv::StorageClass::Input)};
        m.Decorate(in, spv::Decoration::Location, source);
        const Id out{add(corners >= 0 ? m.TypeArray(vec[4], u(3)) : vec[width],
                         spv::StorageClass::Output)};
        m.Decorate(out, spv::Decoration::Location, corners >= 0 ? u32(corners) : source);
        if (corners >= 0) {
            m.Decorate(out, spv::Decoration::Flat);
        }
        attrs.push_back({in, out, width, corners >= 0});
    };
    if (layout.clip >= 0) {
        attribute(u32(layout.clip), MaxEmulatedClipDistances, -1);
    }
    const u32 num_inputs = fs_rt.num_inputs - (fs_rt.clip_distance_emulation ? 1 : 0);
    for (u32 i = 0; i < num_inputs && i < fs_rt.inputs.size(); ++i) {
        if (layout.source[i] < 0) {
            continue;
        }
        const u32 width =
            std::clamp(fs_info.loads.NumComponents(IR::Attribute::Param0 + i), 1u, 4u);
        attribute(u32(layout.source[i]), width, layout.corners[i]);
    }
    Id bary_smooth{}, bary_linear{};
    if (layout.smooth >= 0) {
        bary_smooth = add(vec[3], spv::StorageClass::Output);
        m.Decorate(bary_smooth, spv::Decoration::Location, u32(layout.smooth));
    }
    if (layout.linear >= 0) {
        bary_linear = add(vec[3], spv::StorageClass::Output);
        m.Decorate(bary_linear, spv::Decoration::Location, u32(layout.linear));
        m.Decorate(bary_linear, spv::Decoration::NoPerspective);
    }

    const Id main{m.OpFunction(void_id, spv::FunctionControlMask::MaskNone, m.TypeFunction(void_id))};
    m.AddExecutionMode(main, spv::ExecutionMode::Triangles);
    m.AddExecutionMode(main, spv::ExecutionMode::OutputTriangleStrip);
    m.AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 3U);
    m.AddExecutionMode(main, spv::ExecutionMode::Invocations, 1U);
    m.AddEntryPoint(spv::ExecutionModel::Geometry, main, "main", interfaces);
    m.AddLabel(m.OpLabel());

    const Id in_ptr{m.TypePointer(spv::StorageClass::Input, float_id)};
    const auto widen = [&](Id value, u32 width) {
        if (width == 4) {
            return value;
        }
        std::vector<Id> parts;
        for (u32 c = 0; c < 4; ++c) {
            parts.push_back(c < width ? (width == 1 ? value : m.OpCompositeExtract(float_id, value, c))
                                      : zero);
        }
        return m.OpCompositeConstruct(vec[4], parts);
    };
    // Corners are the same for all three emitted vertices: load them once.
    std::vector<std::array<Id, 3>> corner_values(attrs.size());
    for (size_t a = 0; a < attrs.size(); ++a) {
        if (!attrs[a].corners) {
            continue;
        }
        const Id in_vec{m.TypePointer(spv::StorageClass::Input, vec[attrs[a].width])};
        for (u32 k = 0; k < 3; ++k) {
            corner_values[a][k] =
                widen(m.OpLoad(vec[attrs[a].width], m.OpAccessChain(in_vec, attrs[a].in, u(k))),
                      attrs[a].width);
        }
    }
    const Id primitive{m.OpLoad(int_id, prim_in)};
    for (u32 v = 0; v < 3; ++v) {
        for (u32 k = 0; k < members.size(); ++k) {
            const Id type{members[k]};
            const Id src{m.OpAccessChain(m.TypePointer(spv::StorageClass::Input, type), gl_in, u(v), u(k))};
            const Id dst{m.OpAccessChain(m.TypePointer(spv::StorageClass::Output, type), gl_out, u(k))};
            m.OpStore(dst, m.OpLoad(type, src));
        }
        m.OpStore(prim_out, primitive);
        for (size_t a = 0; a < attrs.size(); ++a) {
            if (attrs[a].corners) {
                const Id out_vec{m.TypePointer(spv::StorageClass::Output, vec[4])};
                for (u32 k = 0; k < 3; ++k) {
                    m.OpStore(m.OpAccessChain(out_vec, attrs[a].out, u(k)), corner_values[a][k]);
                }
            } else {
                const Id in_vec{m.TypePointer(spv::StorageClass::Input, vec[attrs[a].width])};
                m.OpStore(attrs[a].out,
                          m.OpLoad(vec[attrs[a].width], m.OpAccessChain(in_vec, attrs[a].in, u(v))));
            }
        }
        const Id unit{m.ConstantComposite(vec[3], v == 0 ? one : zero, v == 1 ? one : zero,
                                          v == 2 ? one : zero)};
        if (Sirit::ValidId(bary_smooth)) {
            m.OpStore(bary_smooth, unit);
        }
        if (Sirit::ValidId(bary_linear)) {
            m.OpStore(bary_linear, unit);
        }
        m.OpEmitVertex();
    }
    m.OpEndPrimitive();
    m.OpReturn();
    m.OpFunctionEnd();
    (void)in_ptr;
    return m.Assemble();
}

} // namespace Shader::Backend::SPIRV
