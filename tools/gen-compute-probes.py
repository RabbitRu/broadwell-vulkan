"""Fresh SPIR-V 1.0 compute shaders for 3D work groups and shared memory.

ids3d:  local size 8x4x2; out[x + y*W + z*W*H] = x | y << 10 | z << 20 for
        every global invocation id (W, H are specialization-free constants).
shared: local size 4x4x2; each invocation stores li*7 + 3 + group*100 into a
        workgroup-shared array, waits at a barrier, then writes its
        neighbour's value to out[group*32 + li].
"""
import argparse
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parent.parent
grammar = json.loads((root / 'third_party/mesa-26.2.7/src/compiler/spirv/spirv.core.grammar.json').read_text())
opcodes = {item['opname']: item['opcode'] for item in grammar['instructions']}
enums = {kind['kind']: {item['enumerant']: int(str(item['value']), 0) for item in kind.get('enumerants', [])}
         for kind in grammar['operand_kinds']}
W, H = 24, 8


def generate(shared):
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

    def op(name, t, *args):
        result = ident(); emit(body, name, t, result, *args); return result

    def decorate(target, name, *args):
        emit(annotations, 'OpDecorate', target, enums['Decoration'][name], *args)

    void = typ('OpTypeVoid'); uint = typ('OpTypeInt', 32, 0); vec3 = typ('OpTypeVector', uint, 3)
    function = typ('OpTypeFunction', void)

    def constant(value):
        result = ident(); emit(declarations, 'OpConstant', uint, result, value); return result

    inp, uniform = enums['StorageClass']['Input'], enums['StorageClass']['Uniform']
    array = typ('OpTypeRuntimeArray', uint); decorate(array, 'ArrayStride', 4)
    block = typ('OpTypeStruct', array); decorate(block, 'BufferBlock')
    emit(annotations, 'OpMemberDecorate', block, 0, enums['Decoration']['Offset'], 0)
    buffer = ident(); emit(declarations, 'OpVariable', typ('OpTypePointer', uniform, block), buffer, uniform)
    decorate(buffer, 'DescriptorSet', 0); decorate(buffer, 'Binding', 0)
    element = typ('OpTypePointer', uniform, uint)

    def builtin(t, name):
        result = ident(); emit(declarations, 'OpVariable', typ('OpTypePointer', inp, t), result, inp)
        decorate(result, 'BuiltIn', enums['BuiltIn'][name]); return result

    zero = constant(0)
    main = ident()
    if shared:
        local_index, group = builtin(uint, 'LocalInvocationIndex'), builtin(vec3, 'WorkgroupId')
        interfaces = [local_index, group]
        workgroup = enums['StorageClass']['Workgroup']
        shared_array = typ('OpTypeArray', uint, constant(32))
        memory = ident(); emit(declarations, 'OpVariable', typ('OpTypePointer', workgroup, shared_array), memory, workgroup)
        slot = typ('OpTypePointer', workgroup, uint)
        seven, three, hundred, one, count, scope, semantics = (constant(v) for v in (7, 3, 100, 1, 32, 2, 0x108))
        emit(body, 'OpFunction', void, main, 0, function); emit(body, 'OpLabel', ident())
        li = op('OpLoad', uint, local_index)
        g = op('OpCompositeExtract', uint, op('OpLoad', vec3, group), 0)
        value = op('OpIAdd', uint, op('OpIAdd', uint, op('OpIMul', uint, li, seven), three), op('OpIMul', uint, g, hundred))
        emit(body, 'OpStore', op('OpAccessChain', slot, memory, li), value)
        emit(body, 'OpControlBarrier', scope, scope, semantics)
        neighbour = op('OpUMod', uint, op('OpIAdd', uint, li, one), count)
        loaded = op('OpLoad', uint, op('OpAccessChain', slot, memory, neighbour))
        index = op('OpIAdd', uint, op('OpIMul', uint, g, count), li)
        emit(body, 'OpStore', op('OpAccessChain', element, buffer, zero, index), loaded)
        local_size = (4, 4, 2)
    else:
        global_id = builtin(vec3, 'GlobalInvocationId'); interfaces = [global_id]
        width, plane, ten, twenty = (constant(v) for v in (W, W * H, 10, 20))
        emit(body, 'OpFunction', void, main, 0, function); emit(body, 'OpLabel', ident())
        gid = op('OpLoad', vec3, global_id)
        x, y, z = (op('OpCompositeExtract', uint, gid, i) for i in range(3))
        index = op('OpIAdd', uint, op('OpIAdd', uint, x, op('OpIMul', uint, y, width)), op('OpIMul', uint, z, plane))
        value = op('OpBitwiseOr', uint, op('OpBitwiseOr', uint, x, op('OpShiftLeftLogical', uint, y, ten)),
                   op('OpShiftLeftLogical', uint, z, twenty))
        emit(body, 'OpStore', op('OpAccessChain', element, buffer, zero, index), value)
        local_size = (8, 4, 2)
    emit(body, 'OpReturn'); emit(body, 'OpFunctionEnd')
    emit(prefix, 'OpCapability', enums['Capability']['Shader'])
    emit(prefix, 'OpMemoryModel', 0, enums['MemoryModel']['GLSL450'])
    emit(prefix, 'OpEntryPoint', enums['ExecutionModel']['GLCompute'], main, 0x6e69616d, 0, *interfaces)
    emit(prefix, 'OpExecutionMode', main, enums['ExecutionMode']['LocalSize'], *local_size)
    words = [0x07230203, 0x10000, 0, next_id, 0, *prefix, *annotations, *declarations, *body]
    return struct.pack('<' + 'I' * len(words), *words)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root / 'build/icd-shader-inputs')
output = parser.parse_args().output.resolve(); output.mkdir(parents=True, exist_ok=True)
(output / 'compute-ids3d.spv').write_bytes(generate(False))
(output / 'compute-shared.spv').write_bytes(generate(True))
print('Generated SPIR-V compute shaders for 3D groups and shared memory')
