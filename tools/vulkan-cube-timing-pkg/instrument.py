"""Fail closed if the pinned Khronos cube's instrumentation sites change."""
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

def replace(old, new):
    global s
    if s.count(old) != 1:
        raise RuntimeError('expected exactly one instrumentation site: ' + old[:90])
    s = s.replace(old, new)

replace('#define FRAME_LAG 2', '#define FRAME_LAG 2\n#include "brook_timing.h"')
replace('    const VkClearValue clear_values[2] = {',
        '    const VkClearValue clear_values[2] = {')  # Anchor check only.
replace('    demo_name_object(demo, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)submission_resource->cmd, "CubeDrawCommandBuf");',
        '    brook_timing_begin(submission_resource->cmd, demo->current_submission_index);\n'
        '    demo_name_object(demo, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)submission_resource->cmd, "CubeDrawCommandBuf");')
replace('    demo_pop_cb_label(demo, submission_resource->cmd);\n    err = vkEndCommandBuffer(submission_resource->cmd);',
        '    demo_pop_cb_label(demo, submission_resource->cmd);\n'
        '    brook_timing_end(submission_resource->cmd, demo->current_submission_index);\n'
        '    err = vkEndCommandBuffer(submission_resource->cmd);')
replace('    // Ensure no more than FRAME_LAG renderings are outstanding',
        '    brook_timing_init(demo->gpu, demo->device, demo->graphics_queue_family_index);\n'
        '    uint64_t brook_cycle_start = brook_timing_clock();\n'
        '    // Ensure no more than FRAME_LAG renderings are outstanding')
replace('    uint32_t current_swapchain_image_index;\n    do {',
        '    brook_timing_reap(demo->device, demo->current_submission_index);\n'
        '    brook_timing_new(demo->current_submission_index, brook_cycle_start);\n'
        '    uint32_t current_swapchain_image_index;\n    do {')
replace('    SwapchainImageResources current_swapchain_resource = demo->swapchain_resources[current_swapchain_image_index];',
        '    brook_timing_cpu(1);\n'
        '    SwapchainImageResources current_swapchain_resource = demo->swapchain_resources[current_swapchain_image_index];')
replace('    err = vkQueueSubmit(demo->graphics_queue, 1, &submit_info, current_submission.fence);',
        '    brook_timing_cpu(2);\n'
        '    err = vkQueueSubmit(demo->graphics_queue, 1, &submit_info, current_submission.fence);\n'
        '    brook_timing_submitted(demo->current_submission_index, err);\n'
        '    brook_timing_cpu(3);')
replace('    err = vkQueuePresentKHR(demo->present_queue, &present);',
        '    brook_timing_cpu(4);\n'
        '    err = vkQueuePresentKHR(demo->present_queue, &present);\n'
        '    brook_timing_cpu(5);')
replace('    demo->initialized = false;\n    vkDeviceWaitIdle(demo->device);',
        '    demo->initialized = false;\n    vkDeviceWaitIdle(demo->device);\n'
        '    brook_timing_finish(demo->device);')
p.write_text(s)
