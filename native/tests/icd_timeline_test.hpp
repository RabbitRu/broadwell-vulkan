/* Timeline semaphores (VK_KHR_timeline_semaphore, emulated on the binary WDDM
 * sync type): host signal/query/wait, queue signal and wait by value, and a
 * submission that waits for a value the host signals only afterwards. */
#include <thread>
static json test_timelines(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateSemaphore); API(vkDestroySemaphore); API(vkQueueSubmit); API(vkQueueWaitIdle);
   API(vkCreateFence); API(vkDestroyFence); API(vkWaitForFences); API(vkGetFenceStatus);
#undef API
   auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(get(device, "vkSignalSemaphoreKHR"));
   auto wait = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(get(device, "vkWaitSemaphoresKHR"));
   auto value_of = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(get(device, "vkGetSemaphoreCounterValueKHR"));
   require(signal && wait && value_of, "Missing VK_KHR_timeline_semaphore entry points");
   VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue = 5;
   VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; ci.pNext = &type;
   VkSemaphore timeline{}; check(vkCreateSemaphore(device, &ci, nullptr, &timeline), "Create timeline semaphore");
   json cases = json::array();
   uint64_t value = 0;
   check(value_of(device, timeline, &value), "Initial value"); require(value == 5, "Initial timeline value wrong"); cases.push_back("initial value");
   VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; si.semaphore = timeline; si.value = 7;
   check(signal(device, &si), "Host signal"); check(value_of(device, timeline, &value), "Value after host signal");
   require(value == 7, "Host signal not visible"); cases.push_back("host signal");
   VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; wi.semaphoreCount = 1; wi.pSemaphores = &timeline; const uint64_t six = 6; wi.pValues = &six;
   check(wait(device, &wi, 0), "Wait for a reached value"); const uint64_t nine = 9; wi.pValues = &nine;
   require(wait(device, &wi, 1000000) == VK_TIMEOUT, "Wait for an unreached value did not time out"); cases.push_back("host wait, reached and unreached");
   /* Queue: wait for 7, signal 10 (empty submission). */
   auto submit = [&](uint64_t wait_value, uint64_t signal_value, VkFence fence) {
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      ti.waitSemaphoreValueCount = 1; ti.pWaitSemaphoreValues = &wait_value; ti.signalSemaphoreValueCount = 1; ti.pSignalSemaphoreValues = &signal_value;
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.pNext = &ti; s.waitSemaphoreCount = 1; s.pWaitSemaphores = &timeline; s.pWaitDstStageMask = &stage;
      s.signalSemaphoreCount = 1; s.pSignalSemaphores = &timeline;
      check(vkQueueSubmit(queue, 1, &s, fence), "Timeline submission");
   };
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create timeline fence");
   submit(7, 10, fence); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "Queue wait/signal");
   check(value_of(device, timeline, &value), "Value after queue signal"); require(value == 10, "Queue signal not visible"); cases.push_back("queue wait and signal");
   vkDestroyFence(device, fence, nullptr);
   /* Wait-before-signal: the queue waits for 12, the host signals it 200 ms later. */
   check(vkCreateFence(device, &fc, nullptr, &fence), "Create timeline fence");
   submit(12, 13, fence);
   require(vkGetFenceStatus(device, fence) == VK_NOT_READY, "Submission ran before its wait value was signalled");
   std::thread host([&]() { Sleep(200); VkSemaphoreSignalInfo late{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; late.semaphore = timeline; late.value = 12; signal(device, &late); });
   const uint64_t thirteen = 13; wi.pValues = &thirteen;
   check(wait(device, &wi, 10000000000ull), "Wait for the value signalled after the host");
   host.join();
   check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "Fence after wait-before-signal");
   check(value_of(device, timeline, &value), "Final value"); require(value == 13, "Final timeline value wrong"); cases.push_back("wait before signal");
   vkDestroyFence(device, fence, nullptr); check(vkQueueWaitIdle(queue), "Idle after timelines");
   vkDestroySemaphore(device, timeline, nullptr);
   return cases;
}
