#pragma once
#include <wchar.h>
#include "compiler/nir/nir.h"
nir_shader *hasvk_spirv_probe_read(void *memory, const wchar_t *path,
                                  const nir_shader_compiler_options *options);
