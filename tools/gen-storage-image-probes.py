"""Generate Vulkan 1.0 storage-image write/read shaders from Khronos' grammar.

Width and height are specialization constants 0 and 1. The read shader copies
four normalized components per pixel to an SSBO, starting after 16 guard words.
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


def generate(readback):
    next_id = 1
    declarations, annotations, body = [], [], []

    def ident():
        nonlocal next_id
        value = next_id
        next_id += 1
        return value

    def emit(target, name, *operands):
        target.extend([(len(operands) + 1) << 16 | opcodes[name], *operands])

    def define(name, *args):
        result = ident()
        emit(declarations, name, result, *args)
        return result

    def const(typ, value):
        result = ident()
        emit(declarations, 'OpConstant', typ, result, value)
        return result

    def operation(name, typ, *args):
        result = ident()
        emit(body, name, typ, result, *args)
        return result

    void = define('OpTypeVoid')
    uint = define('OpTypeInt', 32, 0)
    floating = define('OpTypeFloat', 32)
    boolean = define('OpTypeBool')
    vec2 = define('OpTypeVector', uint, 2)
    vec3 = define('OpTypeVector', uint, 3)
    vec4 = define('OpTypeVector', floating, 4)
    function_type = define('OpTypeFunction', void)
    image_type = define('OpTypeImage', floating, enums['Dim']['2D'], 0, 0, 0, 2, enums['ImageFormat']['Rgba8'])
    uniform_constant = enums['StorageClass']['UniformConstant']
    image_pointer = define('OpTypePointer', uniform_constant, image_type)
    image = ident(); emit(declarations, 'OpVariable', image_pointer, image, uniform_constant)
    input_storage = enums['StorageClass']['Input']
    input_pointer = define('OpTypePointer', input_storage, vec3)
    global_id = ident(); emit(declarations, 'OpVariable', input_pointer, global_id, input_storage)
    width, height = ident(), ident()
    emit(declarations, 'OpSpecConstant', uint, width, 37)
    emit(declarations, 'OpSpecConstant', uint, height, 23)
    zero, four, sixteen = const(uint, 0), const(uint, 4), const(uint, 16)
    fzero, fone = const(floating, 0), const(floating, 0x3f800000)
    if readback:
        array = define('OpTypeRuntimeArray', uint)
        block = define('OpTypeStruct', array)
        uniform = enums['StorageClass']['Uniform']
        block_pointer = define('OpTypePointer', uniform, block)
        value_pointer = define('OpTypePointer', uniform, uint)
        buffer = ident(); emit(declarations, 'OpVariable', block_pointer, buffer, uniform)
        emit(annotations, 'OpDecorate', array, enums['Decoration']['ArrayStride'], 4)
        emit(annotations, 'OpMemberDecorate', block, 0, enums['Decoration']['Offset'], 0)
        emit(annotations, 'OpDecorate', block, enums['Decoration']['BufferBlock'])
        emit(annotations, 'OpDecorate', buffer, enums['Decoration']['DescriptorSet'], 0)
        emit(annotations, 'OpDecorate', buffer, enums['Decoration']['Binding'], 1)
    main, active_label, end_label = ident(), ident(), ident()
    emit(body, 'OpFunction', void, main, 0, function_type)
    emit(body, 'OpLabel', ident())
    gid = operation('OpLoad', vec3, global_id)
    index = operation('OpCompositeExtract', uint, gid, 0)
    count = operation('OpIMul', uint, width, height)
    active = operation('OpULessThan', boolean, index, count)
    emit(body, 'OpSelectionMerge', end_label, 0)
    emit(body, 'OpBranchConditional', active, active_label, end_label)
    emit(body, 'OpLabel', active_label)
    x = operation('OpUMod', uint, index, width)
    y = operation('OpUDiv', uint, index, width)
    coords = operation('OpCompositeConstruct', vec2, x, y)
    loaded_image = operation('OpLoad', image_type, image)
    if readback:
        color = operation('OpImageRead', vec4, loaded_image, coords)
        base = operation('OpIAdd', uint, operation('OpIMul', uint, index, four), sixteen)
        f255 = const(floating, 0x437f0000)
        for component in range(4):
            value = operation('OpCompositeExtract', floating, color, component)
            value = operation('OpConvertFToU', uint, operation('OpFMul', floating, value, f255))
            dest = operation('OpIAdd', uint, base, const(uint, component))
            pointer = operation('OpAccessChain', value_pointer, buffer, zero, dest)
            emit(body, 'OpStore', pointer, value)
    else:
        three = const(uint, 3)
        xlo, ylo = operation('OpUDiv', uint, width, four), operation('OpUDiv', uint, height, four)
        xhi = operation('OpUDiv', uint, operation('OpIMul', uint, width, three), four)
        yhi = operation('OpUDiv', uint, operation('OpIMul', uint, height, three), four)
        inside_x = operation('OpLogicalAnd', boolean, operation('OpUGreaterThanEqual', boolean, x, xlo), operation('OpULessThan', boolean, x, xhi))
        inside_y = operation('OpLogicalAnd', boolean, operation('OpUGreaterThanEqual', boolean, y, ylo), operation('OpULessThan', boolean, y, yhi))
        green = operation('OpLogicalAnd', boolean, inside_x, inside_y)
        red_value = operation('OpSelect', floating, green, fzero, fone)
        green_value = operation('OpSelect', floating, green, fone, fzero)
        color = operation('OpCompositeConstruct', vec4, red_value, green_value, fzero, fone)
        emit(body, 'OpImageWrite', loaded_image, coords, color)
    emit(body, 'OpBranch', end_label)
    emit(body, 'OpLabel', end_label)
    emit(body, 'OpReturn'); emit(body, 'OpFunctionEnd')
    for ident_value, number in [(width, 0), (height, 1)]:
        emit(annotations, 'OpDecorate', ident_value, enums['Decoration']['SpecId'], number)
    emit(annotations, 'OpDecorate', image, enums['Decoration']['DescriptorSet'], 0)
    emit(annotations, 'OpDecorate', image, enums['Decoration']['Binding'], 0)
    emit(annotations, 'OpDecorate', image, enums['Decoration']['NonWritable' if readback else 'NonReadable'])
    emit(annotations, 'OpDecorate', global_id, enums['Decoration']['BuiltIn'], enums['BuiltIn']['GlobalInvocationId'])
    module = []
    emit(module, 'OpCapability', enums['Capability']['Shader'])
    emit(module, 'OpMemoryModel', 0, enums['MemoryModel']['GLSL450'])
    emit(module, 'OpEntryPoint', enums['ExecutionModel']['GLCompute'], main, 0x6e69616d, 0, global_id)
    emit(module, 'OpExecutionMode', main, enums['ExecutionMode']['LocalSize'], 32, 1, 1)
    words = [0x07230203, 0x10000, 0, next_id, 0, *module, *annotations, *declarations, *body]
    return struct.pack('<' + 'I' * len(words), *words)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root / 'build/icd-shader-inputs')
output = parser.parse_args().output.resolve()
output.mkdir(parents=True, exist_ok=True)
for readback in [False, True]:
    (output / ('storage-image-read.spv' if readback else 'storage-image-write.spv')).write_bytes(generate(readback))
print('Generated SPIR-V storage-image write/read shaders with specialization constants')
