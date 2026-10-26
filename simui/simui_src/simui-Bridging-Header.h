#ifdef __cplusplus
extern "C"
{
#endif

#include "simui_types.h"
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSObject* ObjCBridge_CreateHIDClient(NSObject* device);
NSObject* ObjCBridge_DeviceForUDID(NSString* developerDir, NSString* udid);
bool ObjCBridge_SendHID(NSObject* client, const void* message, size_t size);
#endif

void ObjCBridge_Startup();
void ObjCBridge_Shutdown();
sharedmem_data* ObjCBridge_Loop();
void ObjCBridge_HomeButtonPress();
void ObjCBridge_HomeButtonPressUp();

//#ifdef EYE_CURSOR

int my_LMGetKbdType();

#ifdef __cplusplus
}
#endif
