/* Events on hardware: host set/reset/status, GPU set observed by the host, and
 * vkCmdWaitEvents on an event that is already set (by the host, by an earlier
 * command buffer, earlier in the same command buffer). Each wait must finish. */
static json test_events(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateEvent); API(vkDestroyEvent); API(vkGetEventStatus); API(vkSetEvent); API(vkResetEvent);
   API(vkCmdSetEvent); API(vkCmdResetEvent); API(vkCmdWaitEvents);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
#undef API
   VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   VkEvent event{}; check(vkCreateEvent(device, &ei, nullptr, &event), "Create event");
   require(vkGetEventStatus(device, event) == VK_EVENT_RESET, "New event is not reset");
   check(vkSetEvent(device, event), "Host set event"); require(vkGetEventStatus(device, event) == VK_EVENT_SET, "Host set not visible");
   check(vkResetEvent(device, event), "Host reset event"); require(vkGetEventStatus(device, event) == VK_EVENT_RESET, "Host reset not visible");
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create event command pool");
   auto run = [&](const char *what, auto &&record) {
      std::cerr << "Event case " << what << '\n';
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), what);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), what);
      record(command);
      check(vkEndCommandBuffer(command), what);
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), what);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), what); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), what);
      vkDestroyFence(device, fence, nullptr);
   };
   /* srcStageMask must match how the event was set (HOST_BIT for vkSetEvent). */
   auto wait_from = [&](VkPipelineStageFlags source) { return [&event, &vkCmdWaitEvents, source](VkCommandBuffer command) {
      vkCmdWaitEvents(command, 1, &event, source, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr); }; };
   const auto wait = wait_from(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   json cases = json::array();
   run("GPU set", [&](VkCommandBuffer c) { vkCmdSetEvent(c, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); });
   require(vkGetEventStatus(device, event) == VK_EVENT_SET, "GPU set not visible to the host"); cases.push_back("gpu set, host status");
   run("GPU reset", [&](VkCommandBuffer c) { vkCmdResetEvent(c, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); });
   require(vkGetEventStatus(device, event) == VK_EVENT_RESET, "GPU reset not visible to the host"); cases.push_back("gpu reset, host status");
   check(vkSetEvent(device, event), "Host set before wait");
   run("wait on host-set event", wait_from(VK_PIPELINE_STAGE_HOST_BIT)); cases.push_back("wait on host-set event");
   run("set and wait in one command buffer", [&](VkCommandBuffer c) { vkCmdResetEvent(c, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      vkCmdSetEvent(c, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); wait(c); });
   cases.push_back("set then wait in one command buffer");
   run("wait on event set by the previous command buffer", wait); cases.push_back("wait on event set by an earlier submission");
   vkDestroyCommandPool(device, pool, nullptr); vkDestroyEvent(device, event, nullptr);
   return cases;
}
