"""Fresh SPIR-V 1.0 VS/FS for a right triangle, using Khronos' grammar."""
import argparse
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parent.parent
grammar = json.loads((root / 'third_party/mesa-26.2.7/src/compiler/spirv/spirv.core.grammar.json').read_text())
opcodes = {item['opname']: item['opcode'] for item in grammar['instructions']}
enums = {kind['kind']: {item['enumerant']: int(str(item['value']), 0) for item in kind.get('enumerants', [])}
         for kind in grammar['operand_kinds']}


def generate(fragment, textured=False):
    next_id = 1
    annotations, declarations, body, prefix = [], [], [], []

    def ident():
        nonlocal next_id
        value = next_id
        next_id += 1
        return value

    def emit(target, name, *args):
        target.extend([(len(args) + 1) << 16 | opcodes[name], *args])

    def typ(name, *args):
        result = ident(); emit(declarations, name, result, *args); return result

    def constant(t, value):
        result = ident(); emit(declarations, 'OpConstant', t, result, value); return result

    def op(name, t, *args):
        result = ident(); emit(body, name, t, result, *args); return result

    void = typ('OpTypeVoid')
    floating = typ('OpTypeFloat', 32)
    vec4 = typ('OpTypeVector', floating, 4)
    function = typ('OpTypeFunction', void)
    vec2 = typ('OpTypeVector', floating, 2)
    output = enums['StorageClass']['Output']
    ptr = typ('OpTypePointer', output, vec4)
    dest = ident(); emit(declarations, 'OpVariable', ptr, dest, output)
    interfaces = [dest]
    if textured:
        integer_u = typ('OpTypeInt', 32, 0)
        zero_index = constant(integer_u, 0)
        block = typ('OpTypeStruct', vec4)
        emit(annotations, 'OpDecorate', block, enums['Decoration']['Block'])
        emit(annotations, 'OpMemberDecorate', block, 0, enums['Decoration']['Offset'], 0)
        uniform = enums['StorageClass']['Uniform']
        ubo_ptr, data_ptr = typ('OpTypePointer', uniform, block), typ('OpTypePointer', uniform, vec4)
        ubo = ident(); emit(declarations, 'OpVariable', ubo_ptr, ubo, uniform)
        emit(annotations, 'OpDecorate', ubo, enums['Decoration']['DescriptorSet'], 0)
        emit(annotations, 'OpDecorate', ubo, enums['Decoration']['Binding'], 0)
        uv_storage = enums['StorageClass']['Input'] if fragment else output
        uv_ptr = typ('OpTypePointer', uv_storage, vec2)
        uv = ident(); emit(declarations, 'OpVariable', uv_ptr, uv, uv_storage)
        emit(annotations, 'OpDecorate', uv, enums['Decoration']['Location'], 0); interfaces.append(uv)
        if fragment:
            image_type = typ('OpTypeImage', floating, enums['Dim']['2D'], 0, 0, 0, 1, enums['ImageFormat']['Unknown'])
            sampled_type = typ('OpTypeSampledImage', image_type)
            sampler_ptr = typ('OpTypePointer', enums['StorageClass']['UniformConstant'], sampled_type)
            sampler = ident(); emit(declarations, 'OpVariable', sampler_ptr, sampler, enums['StorageClass']['UniformConstant'])
            emit(annotations, 'OpDecorate', sampler, enums['Decoration']['DescriptorSet'], 0)
            emit(annotations, 'OpDecorate', sampler, enums['Decoration']['Binding'], 1)
    if fragment:
        emit(annotations, 'OpDecorate', dest, enums['Decoration']['Location'], 0)
    else:
        emit(annotations, 'OpDecorate', dest, enums['Decoration']['BuiltIn'], enums['BuiltIn']['Position'])
        integer = typ('OpTypeInt', 32, 1)
        boolean = typ('OpTypeBool')
        inp = enums['StorageClass']['Input']
        vertex_ptr = typ('OpTypePointer', inp, integer)
        vertex = ident(); emit(declarations, 'OpVariable', vertex_ptr, vertex, inp)
        interfaces.append(vertex)
        emit(annotations, 'OpDecorate', vertex, enums['Decoration']['BuiltIn'], enums['BuiltIn']['VertexIndex'])
    zero, one = constant(floating, 0), constant(floating, 0x3f800000)
    main = ident(); emit(body, 'OpFunction', void, main, 0, function); emit(body, 'OpLabel', ident())
    if textured:
        params = op('OpLoad', vec4, op('OpAccessChain', data_ptr, ubo, zero_index))
    if fragment:
        if textured:
            color = op('OpImageSampleImplicitLod', vec4, op('OpLoad', sampled_type, sampler), op('OpLoad', vec2, uv))
            tint = op('OpCompositeExtract', floating, params, 1)
            value = op('OpFMul', vec4, color, op('OpCompositeConstruct', vec4, tint, tint, tint, one))
        else:
            value = op('OpCompositeConstruct', vec4, zero, one, zero, one)
    else:
        index = op('OpLoad', integer, vertex)
        half, minus_half = constant(floating, 0x3f000000), constant(floating, 0xbf000000)
        x = op('OpSelect', floating, op('OpIEqual', boolean, index, constant(integer, 1)), half, minus_half)
        y = op('OpSelect', floating, op('OpIEqual', boolean, index, constant(integer, 2)), half, minus_half)
        depth = op('OpCompositeExtract', floating, params, 0) if textured else zero
        value = op('OpCompositeConstruct', vec4, x, y, depth, one)
        if textured:
            coords = op('OpCompositeConstruct', vec2, op('OpFAdd', floating, x, half), op('OpFAdd', floating, y, half))
            emit(body, 'OpStore', uv, coords)
    emit(body, 'OpStore', dest, value); emit(body, 'OpReturn'); emit(body, 'OpFunctionEnd')
    emit(prefix, 'OpCapability', enums['Capability']['Shader'])
    emit(prefix, 'OpMemoryModel', 0, enums['MemoryModel']['GLSL450'])
    emit(prefix, 'OpEntryPoint', enums['ExecutionModel']['Fragment' if fragment else 'Vertex'], main, 0x6e69616d, 0, *interfaces)
    if fragment:
        emit(prefix, 'OpExecutionMode', main, enums['ExecutionMode']['OriginUpperLeft'])
    words = [0x07230203, 0x10000, 0, next_id, 0, *prefix, *annotations, *declarations, *body]
    return struct.pack('<' + 'I' * len(words), *words)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root / 'build/icd-shader-inputs')
output = parser.parse_args().output.resolve(); output.mkdir(parents=True, exist_ok=True)
for fragment in [False, True]:
    (output / ('triangle.frag.spv' if fragment else 'triangle.vert.spv')).write_bytes(generate(fragment))
    (output / ('textured.frag.spv' if fragment else 'textured.vert.spv')).write_bytes(generate(fragment, True))
print('Generated fresh SPIR-V vertex/fragment modules')
