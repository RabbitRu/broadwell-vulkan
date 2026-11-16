import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
source = root / 'third_party/mesa-26.2.7/src/intel/genxml'
output = root / 'build/native/gen8_pack.h'
output.parent.mkdir(parents=True, exist_ok=True)
symbols = ('INTERFACE_DESCRIPTOR_DATA,RENDER_SURFACE_STATE,STATE_BASE_ADDRESS,'
           'MEDIA_VFE_STATE,MEDIA_INTERFACE_DESCRIPTOR_LOAD,GPGPU_WALKER,PIPE_CONTROL,'
           'PIPELINE_SELECT,MI_LOAD_REGISTER_IMM,MEDIA_STATE_FLUSH,MI_BATCH_BUFFER_END,'
           '3DSTATE_CC_STATE_POINTERS,L3CNTLREG,MEDIA_CURBE_LOAD,MI_BATCH_BUFFER_START')
with output.open('w', encoding='utf-8') as header:
    subprocess.run([sys.executable, str(source / 'gen_pack_header.py'),
                    str(source / 'gen80.xml'), '--include-symbols', symbols],
                   stdout=header, check=True)

# Core Vulkan 1.0 command names for the ICD entry-point coverage report.
import xml.etree.ElementTree as ET
registry = ET.parse(root / 'third_party/Vulkan-Headers-1.3.301/registry/vk.xml').getroot()
feature = next(f for f in registry.iter('feature') if f.get('name') == 'VK_VERSION_1_0')
commands = sorted({c.get('name') for c in feature.iter('command')})
assert len(commands) > 130
(root / 'build/native/vk10_commands.inc').write_text(''.join(f'"{name}",\n' for name in commands), encoding='utf-8')
