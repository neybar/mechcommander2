# Destroying present-wait semaphores is unspecified in core Vulkan

**Status:** open, not reproducing on macOS. Portability risk for M3
(CREDIT_PLAN tasks 12 Linux / 13 Windows). Not a regression — this has been
true since the vk backend was written.

**Backends:** Vulkan only (no GL equivalent — the issue is a WSI/presentation
lifetime rule).

## What

Task 19 gave each swapchain image its own render-completion semaphore, which
fixed `VUID-vkQueueSubmit-pSignalSemaphores-00067`. The remaining gap is
**destruction**, not signalling.

`create_swapchain` (`GameOS/gameos/rendervk/gos_render.cpp`) destroys the old
swapchain and then destroys the old render-done semaphores;
`destroy_render_context` does the same at shutdown. Core Vulkan provides **no
way to know when a presentation engine has finished waiting on a semaphore**:

- Vulkan-Docs issue #2007 — without extensions it is "still not possible to
  safely destroy (or reuse) wait semaphores used with `vkQueuePresentKHR`", and
  neither `vkDeviceWaitIdle` nor `vkQueueWaitIdle` covers a pending present.
- The Vulkan Docs *Swapchain Recreation* sample defers destroying an old
  swapchain's remaining present semaphores until the **first present of the new
  swapchain** has been processed — the opposite of what we do.
- Safe destruction is only specified with `VK_KHR/EXT_swapchain_maintenance1`'s
  present fence.

So the code is doing the best thing available without the extension, but the
comment and log entry originally called it a guarantee. They now don't.

## Why it doesn't bite on macOS today

Two mitigations, the second of which is an implementation property and not a
promise:

1. The only recreate path (`vk_begin_frame` when `swapchain_dirty_`) runs
   `vkDeviceWaitIdle` first.
2. MoltenVK submits presents on an `MTLCommandBuffer` on the same
   `MTLCommandQueue`, so device-idle does drain them.

(2) is what makes this safe here and is exactly what won't hold everywhere.

## Where it could bite

An in-game resolution change sets `swapchain_dirty_`, so the recreate path runs
and destroys all N semaphores. On a WSI where present is asynchronous to the
queue — several Linux and Android drivers — the last present on the retired
swapchain may still hold a wait on one of them, making the `vkDestroySemaphore`
undefined behaviour. Task 3's resolution-switch matrix exercises this path
deliberately, so it is worth re-running that matrix on Linux with validation on.

## Note on evidence

**A clean validation run is not evidence against this.** The validation layer
only enforces present-semaphore destruction rules when `swapchain_maintenance1`
is enabled, so our zero-VUID baseline says nothing about it either way.

## Fix if it does bite

Adopt `VK_KHR_swapchain_maintenance1` (present fence) where available, and fall
back to the current approximation where it isn't. Alternatively defer old-
swapchain semaphore destruction until after the first present on the new
swapchain, per the Khronos sample.

## Sources

- <https://github.com/KhronosGroup/Vulkan-Docs/issues/2007>
- <https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html>
- <https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html>
- <https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_swapchain_maintenance1.html>
