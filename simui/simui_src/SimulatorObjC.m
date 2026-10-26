@import Darwin;
@import ObjectiveC;
@import UniformTypeIdentifiers;
#import <Carbon/Carbon.h>

#include "simui_types.h"

// Use the Objective-C entry points observed in SimulatorKit on Xcode 27.1.
// A fake Swift class with manually padded vtable slots is not ABI compatible.
@protocol XRLegacyHIDClient <NSObject>
- (instancetype)initWithDevice:(NSObject*)device error:(NSError**)error;
- (void)sendWithMessage:(void*)message freeWhenDone:(BOOL)freeWhenDone
       completionQueue:(dispatch_queue_t)queue completion:(void (^)(NSError*))completion;
@end

@protocol XRServiceContext <NSObject>
+ (instancetype)sharedServiceContextForDeveloperDir:(NSString*)directory error:(NSError**)error;
- (NSObject*)defaultDeviceSetWithError:(NSError**)error;
@end
@protocol XRDeviceSet <NSObject>
- (NSDictionary*)devicesByUDID;
@end

NSObject* ObjCBridge_DeviceForUDID(NSString* developerDir, NSString* udid) {
    Class cls = NSClassFromString(@"SimServiceContext");
    if (![cls respondsToSelector:@selector(sharedServiceContextForDeveloperDir:error:)]) {
        NSLog(@"[Sim2OpenXR] CoreSimulator service interface is unavailable.");
        return nil;
    }
    NSError* error = nil;
    id<XRServiceContext> context = [(id<XRServiceContext>)cls sharedServiceContextForDeveloperDir:developerDir error:&error];
    id<XRDeviceSet> set = (id)[context defaultDeviceSetWithError:&error];
    NSObject* device = [set devicesByUDID][[[NSUUID alloc] initWithUUIDString:udid]];
    if (!device) NSLog(@"[Sim2OpenXR] Could not open selected simulator %@: %@", udid, error);
    return device;
}

NSObject* ObjCBridge_CreateHIDClient(NSObject* device) {
    Class cls = NSClassFromString(@"SimulatorKit.SimDeviceLegacyHIDClient");
    SEL initSelector = @selector(initWithDevice:error:);
    SEL sendSelector = @selector(sendWithMessage:freeWhenDone:completionQueue:completion:);
    if (!cls || ![cls instancesRespondToSelector:initSelector] || ![cls instancesRespondToSelector:sendSelector]) {
        NSLog(@"[Sim2OpenXR] Unsupported SimulatorKit HID interface; input disabled.");
        return nil;
    }
    NSError* error = nil;
    id<XRLegacyHIDClient> client = [(id<XRLegacyHIDClient>)[cls alloc] initWithDevice:device error:&error];
    if (!client) NSLog(@"[Sim2OpenXR] HID client creation failed: %@", error);
    return (NSObject*)client;
}

bool ObjCBridge_SendHID(NSObject* client, const void* message, size_t size) {
    if (!client || !message || !size) return false;
    // SimulatorKit may consume the payload asynchronously. Give it its own
    // malloc allocation and request that it release the payload when finished.
    void* copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, message, size);
    [(id<XRLegacyHIDClient>)client sendWithMessage:copy freeWhenDone:YES completionQueue:nil completion:nil];
    return true;
}

void *shm_addr = NULL;

int redirect_nslog(const char *prefix, const char *buffer, int size)
{
    NSLog(@"%s (%d bytes): %.*s", prefix, size, size, buffer);
    return size;
}

int stderr_redirect_nslog(void *inFD, const char *buffer, int size)
{
    return redirect_nslog("stderr", buffer, size);
}

int stdout_redirect_nslog(void *inFD, const char *buffer, int size)
{
    return redirect_nslog("stdout", buffer, size);
}

OBJC_EXPORT void ObjCBridge_Startup()
{
    setlinebuf(stdout);
    setlinebuf(stderr);
    stdout->_write = stdout_redirect_nslog;
    stderr->_write = stderr_redirect_nslog;

    // Open the shared memory, with required access mode and permissions.
    // This is analagous to how we open file with open()
    int fd = shm_open("/tmp/Sim2OpenXR_shmem", O_CREAT | O_RDWR /* open flags */, S_IRUSR | S_IWUSR /* mode */);
    if (fd < 0) {
        NSLog(@"[Sim2OpenXR] Shared memory open failed: %s", strerror(errno));
        return;
    }

    // extend or shrink to the required size (specified in bytes)
    if (ftruncate(fd, 1048576) < 0) {
        NSLog(@"[Sim2OpenXR] Shared memory resize failed: %s", strerror(errno));
        close(fd);
        return;
    }

    // map the shared memory to an address in the virtual address space
    // with the file descriptor of the shared memory and necessary protection.
    // The flags to mmap must be set to MAP_SHARED for other process to access.
    shm_addr = mmap(NULL, 1048576, PROT_READ | PROT_WRITE /*protection*/, MAP_SHARED /*flags*/, fd, 0);
    close(fd);

    // read from/write to the virtual address backing the shared memory

    if (shm_addr == MAP_FAILED) {
        NSLog(@"[Sim2OpenXR] Shared memory setup failed: %s", strerror(errno));
        shm_addr = NULL;
        return;
    }
    printf("[Sim2OpenXR] Input session ready; waiting for tracked frames.\n");


}

OBJC_EXPORT void ObjCBridge_Shutdown()
{
    // when done, unmap the virtual address mapped to the shared memory
    if (shm_addr) munmap(shm_addr, 1048576);
    shm_addr = NULL;

    // remove the shared memory when it is no longer needed.
    shm_unlink("/tmp/Sim2OpenXR_shmem");
}

OBJC_EXPORT sharedmem_data* ObjCBridge_Loop()
{
    static sharedmem_data empty = {0};
    if (!shm_addr) return &empty;
    sharedmem_data* ret = (sharedmem_data*)shm_addr;
    //printf("%f %f %f\n", ret->l_x, ret->l_y, ret->l_z);
    return ret;
}

OBJC_EXPORT void ObjCBridge_HomeButtonPress()
{
    CGEventRef ref = CGEventCreateKeyboardEvent(NULL, 0x4 /* H */, 1);
    CGEventSetFlags( ref, kCGEventFlagMaskCommand | kCGEventFlagMaskShift  );
    CGEventPost(kCGHIDEventTap, ref);
    
    // Doesn't work...
    /*ProcessSerialNumber psn;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kCurrentProcess;
    CGEventPostToPSN(&psn, ref);*/


    CFRelease(ref);
}

OBJC_EXPORT void ObjCBridge_HomeButtonPressUp()
{
    CGEventRef ref = CGEventCreateKeyboardEvent(NULL, 0x4 /* H */, 0);
    CGEventSetFlags( ref, kCGEventFlagMaskCommand | kCGEventFlagMaskShift );
    CGEventPost(kCGHIDEventTap, ref);
    CFRelease(ref);
}

OBJC_EXPORT int my_LMGetKbdType() {
    return LMGetKbdType();
}
