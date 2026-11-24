#include "gen8_compute.h"
#include <stdbool.h>
#include <string.h>
#define __gen_address_type uint64_t
#define __gen_user_data void
static inline uint64_t
__gen_combine_address(void *data, void *location, uint64_t address, uint32_t delta)
{
   (void)data; (void)location;
   return address + delta;
}
#include "gen8_pack.h"

static uint64_t canonical_gpu(uint64_t address)
{
   return address & (1ull << 47) ? address | 0xffff000000000000ull : address;
}

size_t hasvk_gen8_batch_jump(void *commands, uint64_t target)
{
   if (!commands || (target & 7)) return 0;
   struct GFX8_MI_BATCH_BUFFER_START jump = {GFX8_MI_BATCH_BUFFER_START_header};
   jump.AddressSpaceIndicator = ASI_PPGTT;
   jump.BatchBufferStartAddress = canonical_gpu(target);
   GFX8_MI_BATCH_BUFFER_START_pack(NULL, commands, &jump);
   ((uint32_t *)commands)[3] = 0;
   return 16;
}

size_t hasvk_gen8_constant_compute(const struct hasvk_gen8_compute_info *info,
                                  void *commands, size_t command_capacity,
                                  void *dynamic_state, void *surface_state)
{
   if (!info || !commands || !dynamic_state || !surface_state || command_capacity < 1024 ||
       !info->max_threads || info->max_threads > 512 || info->per_thread_regs > 1 || (info->kernel_offset & 63) ||
       (info->simd_width != 8 && info->simd_width != 16 && info->simd_width != 32)) return 0;
   if (!info->dispatch_threads || info->dispatch_threads > 8 || !info->group_count_x ||
       info->group_count_x > 65535 || info->output_bytes < 4 || (info->output_bytes & 3) ||
       info->output_bytes > 128 * 1024 * 1024 || (info->instruction_offset & 4095) ||
       (info->dynamic_offset & 4095) || (info->surface_offset & 4095) ||
       info->instruction_offset > 0x3ffff000 || info->dynamic_offset > 0x3ffff000 ||
       info->surface_offset > 0x3ffff000) return 0;
   memset(commands, 0, command_capacity);
   memset(dynamic_state, 0, 4096);
   memset(surface_state, 0, 4096);
   struct GFX8_RENDER_SURFACE_STATE surface = {0};
   surface.SurfaceType = SURFTYPE_BUFFER;
   surface.SurfaceFormat = 0x1ff; /* ISL_FORMAT_RAW; byte-addressed SSBO */
   surface.SurfaceHorizontalAlignment = HALIGN_4;
   surface.SurfaceVerticalAlignment = VALIGN_4;
   surface.MOCS = 0x78; /* Mesa ISL's Broadwell internal WB cache policy */
   /* RAW byte size split exactly as isl_buffer_fill_state_s. */
   uint32_t surface_size = info->output_bytes - 1;
   surface.Width = surface_size & 127;
   surface.Height = (surface_size >> 7) & 16383;
   surface.Depth = (surface_size >> 21) & 1023;
   surface.SurfaceBaseAddress = canonical_gpu(info->output_base);
   surface.ShaderChannelSelectRed = SCS_RED;
   surface.ShaderChannelSelectGreen = SCS_GREEN;
   surface.ShaderChannelSelectBlue = SCS_BLUE;
   surface.ShaderChannelSelectAlpha = SCS_ALPHA;
   ((uint32_t *)surface_state)[0] = info->surface_offset + 64;
   GFX8_RENDER_SURFACE_STATE_pack(NULL, (uint8_t *)surface_state + 64, &surface);
   struct GFX8_INTERFACE_DESCRIPTOR_DATA descriptor = {0};
   descriptor.KernelStartPointer = info->instruction_offset + info->kernel_offset;
   descriptor.BindingTableEntryCount = 1;
   descriptor.BindingTablePointer = info->surface_offset;
   descriptor.ConstantURBEntryReadLength = info->per_thread_regs;
   descriptor.NumberofThreadsinGPGPUThreadGroup = info->dispatch_threads;
   GFX8_INTERFACE_DESCRIPTOR_DATA_pack(NULL, dynamic_state, &descriptor);
   uint32_t *dw = commands;
   size_t count = 0;
#define PUSH(type, value) do { \
   if ((count + GFX8_##type##_length) * 4 > command_capacity) return 0; \
   GFX8_##type##_pack(NULL, dw + count, &(value)); \
   count += GFX8_##type##_length; \
} while (0)
   struct GFX8_PIPELINE_SELECT select3d = {GFX8_PIPELINE_SELECT_header};
   select3d.PipelineSelection = _3D;
   PUSH(PIPELINE_SELECT, select3d);
   struct GFX8_3DSTATE_CC_STATE_POINTERS cc = {GFX8_3DSTATE_CC_STATE_POINTERS_header};
   PUSH(3DSTATE_CC_STATE_POINTERS, cc);
   struct GFX8_PIPE_CONTROL flush = {GFX8_PIPE_CONTROL_header};
   flush.DepthCacheFlushEnable = true;
   flush.RenderTargetCacheFlushEnable = true;
   flush.DCFlushEnable = true;
   flush.CommandStreamerStallEnable = true;
   PUSH(PIPE_CONTROL, flush);
   struct GFX8_PIPE_CONTROL invalidate = {GFX8_PIPE_CONTROL_header};
   invalidate.StateCacheInvalidationEnable = true;
   invalidate.ConstantCacheInvalidationEnable = true;
   invalidate.TextureCacheInvalidationEnable = true;
   invalidate.InstructionCacheInvalidateEnable = true;
   PUSH(PIPE_CONTROL, invalidate);
   struct GFX8_PIPELINE_SELECT select_compute = {GFX8_PIPELINE_SELECT_header};
   select_compute.PipelineSelection = GPGPU;
   PUSH(PIPELINE_SELECT, select_compute);
   /* WaSendDummyVFEafterPipelineSelect with the values of Intel's Windows
    * driver: without it the first dispatch of a context hangs now and then
    * while another process renders in 3D (mesa/graphics_sources.py,
    * dummy_vfe_after_pipeline_select). */
   struct GFX8_MEDIA_VFE_STATE dummy_vfe = {GFX8_MEDIA_VFE_STATE_header};
   dummy_vfe.NumberofURBEntries = 1; dummy_vfe.BypassGatewayControl = true;
   PUSH(MEDIA_VFE_STATE, dummy_vfe);
   struct GFX8_L3CNTLREG l3 = {0};
   l3.URBAllocation = 48; l3.AllAllocation = 48; /* Validated BDW configuration in Mesa */
   uint32_t l3_value;
   GFX8_L3CNTLREG_pack(NULL, &l3_value, &l3);
   struct GFX8_MI_LOAD_REGISTER_IMM lri = {GFX8_MI_LOAD_REGISTER_IMM_header};
   lri.RegisterOffset = GFX8_L3CNTLREG_num; lri.DataDWord = l3_value;
   PUSH(MI_LOAD_REGISTER_IMM, lri);
   struct GFX8_STATE_BASE_ADDRESS bases = {GFX8_STATE_BASE_ADDRESS_header};
   bases.GeneralStateMOCS = bases.StatelessDataPortAccessMOCS = 0x78;
   bases.SurfaceStateMOCS = bases.DynamicStateMOCS = bases.IndirectObjectMOCS = bases.InstructionMOCS = 0x78;
   bases.SurfaceStateBaseAddressModifyEnable = true;
   bases.SurfaceStateBaseAddress = canonical_gpu(info->surface_base);
   bases.DynamicStateBaseAddressModifyEnable = true;
   bases.DynamicStateBaseAddress = canonical_gpu(info->dynamic_base);
   bases.IndirectObjectBaseAddressModifyEnable = true;
   bases.IndirectObjectBaseAddress = canonical_gpu(info->dynamic_base);
   bases.InstructionBaseAddressModifyEnable = true;
   bases.InstructionBaseAddress = canonical_gpu(info->instruction_base);
   bases.DynamicStateBufferSizeModifyEnable = true; bases.DynamicStateBufferSize = info->dynamic_offset / 4096 + 1;
   bases.IndirectObjectBufferSizeModifyEnable = true; bases.IndirectObjectBufferSize = 1;
   bases.InstructionBuffersizeModifyEnable = true; bases.InstructionBufferSize = info->instruction_offset / 4096 + 1;
   PUSH(STATE_BASE_ADDRESS, bases);
   PUSH(PIPE_CONTROL, invalidate);
   PUSH(PIPE_CONTROL, flush);
   struct GFX8_MEDIA_VFE_STATE vfe = {GFX8_MEDIA_VFE_STATE_header};
   vfe.MaximumNumberofThreads = info->max_threads - 1;
   vfe.NumberofURBEntries = 2; vfe.URBEntryAllocationSize = 2;
   uint32_t curbe_regs = info->per_thread_regs * info->dispatch_threads;
   vfe.CURBEAllocationSize = (curbe_regs + 1) & ~1u;
   vfe.ResetGatewayTimer = true; vfe.BypassGatewayControl = true;
   PUSH(MEDIA_VFE_STATE, vfe);
   struct GFX8_MEDIA_INTERFACE_DESCRIPTOR_LOAD load = {GFX8_MEDIA_INTERFACE_DESCRIPTOR_LOAD_header};
   load.InterfaceDescriptorTotalLength = 32;
   load.InterfaceDescriptorDataStartAddress = info->dynamic_offset;
   PUSH(MEDIA_INTERFACE_DESCRIPTOR_LOAD, load);
   if (info->per_thread_regs) {
      /* ELK's only supported push parameter is the subgroup ID, with a
       * separate 32-byte register for each hardware thread in a workgroup. */
      for (uint32_t t = 0; t < info->dispatch_threads; ++t)
         ((uint32_t *)((uint8_t *)dynamic_state + 64 + t * 32))[0] = t;
      struct GFX8_MEDIA_CURBE_LOAD curbe = {GFX8_MEDIA_CURBE_LOAD_header};
      curbe.CURBETotalDataLength = ((curbe_regs + 1) & ~1u) * 32;
      curbe.CURBEDataStartAddress = info->dynamic_offset + 64;
      PUSH(MEDIA_CURBE_LOAD, curbe);
   }
   struct GFX8_GPGPU_WALKER walker = {GFX8_GPGPU_WALKER_header};
   walker.SIMDSize = info->simd_width / 16;
   walker.ThreadWidthCounterMaximum = info->dispatch_threads - 1;
   walker.ThreadGroupIDXDimension = info->group_count_x;
   walker.ThreadGroupIDYDimension = walker.ThreadGroupIDZDimension = 1;
   walker.RightExecutionMask = info->right_mask; walker.BottomExecutionMask = 0xffffffff;
   PUSH(GPGPU_WALKER, walker);
   struct GFX8_MEDIA_STATE_FLUSH media = {GFX8_MEDIA_STATE_FLUSH_header};
   PUSH(MEDIA_STATE_FLUSH, media);
   flush.PostSyncOperation = WriteImmediateData;
   flush.Address = canonical_gpu(info->scratch_base); flush.ImmediateData = 1;
   PUSH(PIPE_CONTROL, flush);
   struct GFX8_MI_BATCH_BUFFER_END end = {GFX8_MI_BATCH_BUFFER_END_header};
   PUSH(MI_BATCH_BUFFER_END, end);
   if (count & 1) dw[count++] = 0;
   return count * 4;
#undef PUSH
}
