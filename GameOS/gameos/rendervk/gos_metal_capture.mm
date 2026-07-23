// Metal frame capture for the Vulkan backend. See gos_metal_capture.h.
//
// The MTLDevice comes out of MoltenVK through VK_EXT_metal_objects; capturing
// the device captures every queue on it, so that one handle is all Xcode needs.

#include "gos_metal_capture.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <vulkan/vulkan_metal.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

namespace graphics {

namespace {

const char* const CAPTURE_AT_ENV = "MC2_VK_CAPTURE_AT_SECS";
const char* const CAPTURE_FILE_ENV = "MC2_VK_CAPTURE_FILE";
const char* const DEFAULT_OUT = "/tmp/mc2-vk.gputrace";

id<MTLDevice> g_mtl_device = nil;
VkDevice      g_device = VK_NULL_HANDLE;
bool          g_ready = false;      // init succeeded, a capture is still owed
bool          g_capturing = false;  // between start and stop
double        g_at_secs = 0.0;
double        g_t0 = 0.0;
char          g_out_path[1024] = {0};

double now_secs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

} // namespace

bool metal_capture_requested()
{
    const char* at = getenv(CAPTURE_AT_ENV);
    return at && *at;
}

void metal_capture_init(VkDevice device, bool metal_objects_enabled)
{
    if(!metal_capture_requested())
        return;

    g_at_secs = atof(getenv(CAPTURE_AT_ENV));

    if(!metal_objects_enabled) {
        fprintf(stderr, "[VKCAP] VK_EXT_metal_objects unavailable -- no capture\n");
        return;
    }

    PFN_vkExportMetalObjectsEXT export_objects =
        (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT");
    if(!export_objects) {
        fprintf(stderr, "[VKCAP] vkExportMetalObjectsEXT missing -- no capture\n");
        return;
    }

    VkExportMetalDeviceInfoEXT dev_info = {};
    dev_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
    VkExportMetalObjectsInfoEXT obj_info = {};
    obj_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
    obj_info.pNext = &dev_info;
    export_objects(device, &obj_info);

    g_mtl_device = dev_info.mtlDevice;
    if(!g_mtl_device) {
        fprintf(stderr, "[VKCAP] MoltenVK returned no MTLDevice -- no capture\n");
        return;
    }

    const char* out = getenv(CAPTURE_FILE_ENV);
    snprintf(g_out_path, sizeof(g_out_path), "%s", (out && *out) ? out : DEFAULT_OUT);

    // Metal refuses to overwrite an existing trace, and it fails at
    // stopCapture -- long after the frame we wanted is gone. Fail loudly now.
    struct stat st;
    if(0 == stat(g_out_path, &st)) {
        fprintf(stderr, "[VKCAP] %s already exists -- move it aside first, no capture\n", g_out_path);
        return;
    }

    MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
    if(![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
        fprintf(stderr, "[VKCAP] GPU trace documents not supported here -- relaunch with "
                        "METAL_CAPTURE_ENABLED=1 (and if that is already set, the binary needs "
                        "the com.apple.security.get-task-allow entitlement)\n");
        return;
    }

    g_device = device;
    g_ready = true;
    g_t0 = now_secs();
    printf("[VKCAP] armed: first frame after %.1fs -> %s\n", g_at_secs, g_out_path);
}

void metal_capture_frame_begin()
{
    if(!g_ready || g_capturing)
        return;
    if(now_secs() - g_t0 < g_at_secs)
        return;

    MTLCaptureDescriptor* desc = [[MTLCaptureDescriptor alloc] init];
    desc.captureObject = g_mtl_device;
    desc.destination = MTLCaptureDestinationGPUTraceDocument;
    desc.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:g_out_path]];

    NSError* err = nil;
    BOOL ok = [[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:desc error:&err];
    [desc release];

    if(!ok) {
        fprintf(stderr, "[VKCAP] startCapture failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "unknown error");
        g_ready = false; // one shot; do not retry every frame
        return;
    }

    g_capturing = true;
    printf("[VKCAP] capturing this frame (t=%.1fs)\n", now_secs() - g_t0);
}

void metal_capture_frame_end()
{
    if(!g_capturing)
        return;

    // The present is submitted but MoltenVK's own command buffer for it may
    // still be in flight; stopping the capture under it truncates the trace.
    vkDeviceWaitIdle(g_device);
    [[MTLCaptureManager sharedCaptureManager] stopCapture];

    g_capturing = false;
    g_ready = false;
    printf("[VKCAP] wrote %s -- open it in Xcode\n", g_out_path);
}

} // namespace graphics
