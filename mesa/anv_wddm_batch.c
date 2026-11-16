/* Pinned-VA batch bookkeeping for the Windows transport. No Linux relocations
 * or ioctl submission: every BO already has its final WDDM GPU address. */
#include "anv_wddm.h"

VkResult anv_reloc_list_init(struct anv_reloc_list *list, const VkAllocationCallbacks *alloc)
{
   (void)alloc; memset(list, 0, sizeof(*list)); return VK_SUCCESS;
}
void anv_reloc_list_finish(struct anv_reloc_list *list, const VkAllocationCallbacks *alloc)
{
   vk_free(alloc, list->relocs); vk_free(alloc, list->reloc_bos); vk_free(alloc, list->deps);
}
VkResult anv_reloc_list_add_bo(struct anv_reloc_list *list, const VkAllocationCallbacks *alloc, struct anv_bo *bo)
{
   if (!bo || bo->is_wrapper || !anv_bo_is_pinned(bo) || !bo->gem_handle)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   uint32_t needed = bo->gem_handle / BITSET_WORDBITS + 1;
   if (needed > list->dep_words) {
      uint32_t length = MAX2(needed, MAX2(32, list->dep_words * 2));
      BITSET_WORD *deps = vk_realloc(alloc, list->deps, (size_t)length * sizeof(*deps), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!deps) return VK_ERROR_OUT_OF_HOST_MEMORY;
      __bitset_zero(deps + list->dep_words, length - list->dep_words);
      list->deps = deps; list->dep_words = length;
   }
   BITSET_SET(list->deps, bo->gem_handle);
   return VK_SUCCESS;
}
VkResult anv_reloc_list_add(struct anv_reloc_list *list, const VkAllocationCallbacks *alloc,
   uint32_t offset, struct anv_bo *bo, uint32_t delta, uint64_t *address)
{
   (void)offset;
   VkResult result = anv_reloc_list_add_bo(list, alloc, bo);
   if (result == VK_SUCCESS && address) *address = intel_canonical_address(bo->offset + delta);
   return result;
}
void *anv_batch_emit_dwords(struct anv_batch *batch, int count)
{
   if (count < 0) { anv_batch_set_error(batch, VK_ERROR_OUT_OF_HOST_MEMORY); return NULL; }
   if ((char *)batch->next + count * 4 > (char *)batch->end) {
      VkResult result = batch->extend_cb ? batch->extend_cb(batch, batch->user_data) : VK_ERROR_OUT_OF_HOST_MEMORY;
      if (result != VK_SUCCESS) { anv_batch_set_error(batch, result); return NULL; }
   }
   void *result = batch->next; batch->next = (char *)batch->next + count * 4; return result;
}
struct anv_address anv_batch_address(struct anv_batch *batch, void *location)
{
   assert(location >= batch->start && location <= batch->next);
   return anv_address_add(batch->start_addr, (char *)location - (char *)batch->start);
}
void anv_batch_emit_batch(struct anv_batch *batch, struct anv_batch *other)
{
   const size_t size = (char *)other->next - (char *)other->start;
   void *destination = anv_batch_emit_dwords(batch, size / 4);
   if (!destination) return;
   memcpy(destination, other->start, size);
   if (!other->relocs || !other->relocs->dep_words) return;
   if (batch->relocs->dep_words < other->relocs->dep_words) {
      uint32_t length = other->relocs->dep_words;
      BITSET_WORD *deps = vk_realloc(batch->alloc, batch->relocs->deps, (size_t)length * sizeof(*deps), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!deps) { anv_batch_set_error(batch, VK_ERROR_OUT_OF_HOST_MEMORY); return; }
      __bitset_zero(deps + batch->relocs->dep_words, length - batch->relocs->dep_words);
      batch->relocs->deps = deps; batch->relocs->dep_words = length;
   }
   for (unsigned word = 0; word < other->relocs->dep_words; ++word)
      batch->relocs->deps[word] |= other->relocs->deps[word];
}
