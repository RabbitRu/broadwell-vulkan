"""Generate small Vulkan 1.0 SPIR-V compute modules using Khronos' grammar.

Each invocation reads buffer[0], computes an integer transform, and stores the
result. CPU reference values are independent of compiler output and GPU data.
"""
import hashlib
import json
import pathlib
import secrets
import struct
import argparse

ROOT = pathlib.Path(__file__).resolve().parent.parent
GRAMMAR = ROOT / 'third_party/mesa-26.2.7/src/compiler/spirv/spirv.core.grammar.json'
grammar = json.loads(GRAMMAR.read_text(encoding='utf-8'))
opcodes = {entry['opname']: entry['opcode'] for entry in grammar['instructions']}
enums = {entry['kind']: {value['enumerant']: int(str(value['value']), 0)
                       for value in entry.get('enumerants', [])}
         for entry in grammar['operand_kinds']}
MASK = 0xffffffff


def string_words(text):
    raw = text.encode() + b'\0'
    raw += b'\0' * (-len(raw) % 4)
    return list(struct.unpack('<' + 'I' * (len(raw) // 4), raw))


def generate(algorithm, binding=0, local_size=1, array_mode=False):
    next_id = 1

    def ident():
        nonlocal next_id
        result = next_id
        next_id += 1
        return result

    main, void, uint, function_type, array, block, ptr_block, ptr_uint, buffer = [ident() for _ in range(9)]
    zero = ident()
    constants = {0: zero}
    declarations, body = [], []

    def emit(target, opcode, *operands):
        target.extend([(1 + len(operands)) << 16 | opcodes[opcode], *operands])

    def constant(value):
        if value not in constants:
            constants[value] = ident()
            emit(declarations, 'OpConstant', uint, constants[value], value)
        return constants[value]

    def operation(opcode, left, right):
        result = ident()
        emit(body, opcode, uint, result, left, right)
        return result

    emit(declarations, 'OpTypeVoid', void)
    emit(declarations, 'OpTypeInt', uint, 32, 0)
    emit(declarations, 'OpTypeFunction', function_type, void)
    emit(declarations, 'OpTypeRuntimeArray', array, uint)
    emit(declarations, 'OpTypeStruct', block, array)
    uniform = enums['StorageClass']['Uniform']
    emit(declarations, 'OpTypePointer', ptr_block, uniform, block)
    emit(declarations, 'OpTypePointer', ptr_uint, uniform, uint)
    emit(declarations, 'OpConstant', uint, zero, 0)
    if array_mode:
        boolean, vec3, ptr_input, global_id = [ident() for _ in range(4)]
        emit(declarations, 'OpTypeBool', boolean)
        emit(declarations, 'OpTypeVector', vec3, uint, 3)
        emit(declarations, 'OpTypePointer', ptr_input, enums['StorageClass']['Input'], vec3)
        emit(declarations, 'OpVariable', ptr_input, global_id, enums['StorageClass']['Input'])
    emit(body, 'OpFunction', void, main, enums['FunctionControl']['None'], function_type)
    emit(body, 'OpLabel', ident())
    if array_mode:
        gid, index, size_ptr, count, active = [ident() for _ in range(5)]
        then_label, end_label = ident(), ident()
        emit(body, 'OpLoad', vec3, gid, global_id)
        emit(body, 'OpCompositeExtract', uint, index, gid, 0)
        emit(body, 'OpAccessChain', ptr_uint, size_ptr, buffer, zero, zero)
        emit(body, 'OpLoad', uint, count, size_ptr)
        emit(body, 'OpULessThan', boolean, active, index, count)
        emit(body, 'OpSelectionMerge', end_label, enums['SelectionControl']['None'])
        emit(body, 'OpBranchConditional', active, then_label, end_label)
        emit(body, 'OpLabel', then_label)
        array_index = operation('OpIAdd', index, constant(16))
    else:
        array_index = zero
    pointer = ident()
    emit(body, 'OpAccessChain', ptr_uint, pointer, buffer, zero, array_index)
    value = ident()
    emit(body, 'OpLoad', uint, value, pointer)
    if algorithm == 'xor32':
        value = operation('OpBitwiseXor', value, constant(0x9e3779b9))
    elif algorithm == 'lcg32':
        value = operation('OpIMul', value, constant(1664525))
        value = operation('OpIAdd', value, constant(1013904223))
    elif algorithm == 'mix32':
        for shift, multiplier in [(16, 0x7feb352d), (15, 0x846ca68b), (16, None)]:
            shifted = operation('OpShiftRightLogical', value, constant(shift))
            value = operation('OpBitwiseXor', value, shifted)
            if multiplier is not None:
                value = operation('OpIMul', value, constant(multiplier))
    else:
        raise ValueError(algorithm)
    emit(body, 'OpStore', pointer, value)
    if array_mode:
        emit(body, 'OpBranch', end_label)
        emit(body, 'OpLabel', end_label)
    emit(body, 'OpReturn')
    emit(body, 'OpFunctionEnd')
    emit(declarations, 'OpVariable', ptr_block, buffer, uniform)
    module = []
    emit(module, 'OpCapability', enums['Capability']['Shader'])
    emit(module, 'OpMemoryModel', enums['AddressingModel']['Logical'], enums['MemoryModel']['GLSL450'])
    interface = [global_id] if array_mode else []
    emit(module, 'OpEntryPoint', enums['ExecutionModel']['GLCompute'], main, *string_words('main'), *interface)
    emit(module, 'OpExecutionMode', main, enums['ExecutionMode']['LocalSize'], local_size, 1, 1)
    decoration = enums['Decoration']
    emit(module, 'OpDecorate', array, decoration['ArrayStride'], 4)
    emit(module, 'OpMemberDecorate', block, 0, decoration['Offset'], 0)
    emit(module, 'OpDecorate', block, decoration['BufferBlock'])
    emit(module, 'OpDecorate', buffer, decoration['DescriptorSet'], 0)
    emit(module, 'OpDecorate', buffer, decoration['Binding'], binding)
    if array_mode:
        emit(module, 'OpDecorate', global_id, decoration['BuiltIn'], enums['BuiltIn']['GlobalInvocationId'])
    words = [0x07230203, 0x00010000, 0, next_id, 0, *module, *declarations, *body]
    return struct.pack('<' + 'I' * len(words), *words)


def reference(algorithm, value):
    if algorithm == 'xor32':
        return value ^ 0x9e3779b9
    if algorithm == 'lcg32':
        return (value * 1664525 + 1013904223) & MASK
    value ^= value >> 16
    value = value * 0x7feb352d & MASK
    value ^= value >> 15
    value = value * 0x846ca68b & MASK
    return value ^ (value >> 16)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'build/spirv')
output = parser.parse_args().output.resolve()
output.mkdir(parents=True, exist_ok=True)
cases = []
for algorithm in ['xor32', 'lcg32', 'mix32']:
    module = generate(algorithm)
    (output / f'{algorithm}.spv').write_bytes(module)
    for iteration in range(3):
        initial = secrets.randbits(32)
        while reference(algorithm, initial) == initial:
            initial = secrets.randbits(32)
        cases.append({'name': f'{algorithm}-{iteration}', 'algorithm': algorithm,
                      'module': f'{algorithm}.spv', 'moduleSha256': hashlib.sha256(module).hexdigest(),
                      'initialValue': initial, 'expectedValue': reference(algorithm, initial)})
(output / 'unsupported-binding.spv').write_bytes(generate('xor32', binding=1))
(output / 'invalid.spv').write_bytes(b'\0' * 20)
for algorithm in ['xor32', 'lcg32', 'mix32']:
    for local_size in [7, 32, 64]:
        module_name = f'{algorithm}-local{local_size}.spv'
        module = generate(algorithm, local_size=local_size, array_mode=True)
        (output / module_name).write_bytes(module)
        for count in [1, local_size - 1, local_size, local_size + 1, 257, 16381]:
            name = f'{algorithm}-local{local_size}-count{count}'
            initial = [secrets.randbits(32) for _ in range(count)]
            vectors = {'initialWords': initial, 'expectedWords': [reference(algorithm, v) for v in initial]}
            case_dir = output / name
            case_dir.mkdir(exist_ok=True)
            (case_dir / 'vectors.json').write_text(json.dumps(vectors), encoding='utf-8')
            cases.append({'name': name, 'algorithm': algorithm, 'module': module_name,
                          'moduleSha256': hashlib.sha256(module).hexdigest(),
                          'elementCount': count, 'localSize': local_size,
                          'expectedValue': vectors['expectedWords'][0]})
(output / 'cases.json').write_text(json.dumps({'grammarSha256': hashlib.sha256(GRAMMAR.read_bytes()).hexdigest(),
                                              'cases': cases}, indent=2), encoding='utf-8')
print(f'Generated {len(cases)} SPIR-V compute cases and two rejection cases')
