#include "GLADriver.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

// Factory UUID must match Info.plist AudioServerPlugIn_FactoryUUIDs entry.
#define kGLADriverFactoryUUID "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"

static GLADriver* gDriver = nullptr;

extern "C" __attribute__((visibility("default")))
void* GLAInjector_Create (CFAllocatorRef /*allocator*/, CFUUIDRef requestedTypeUUID)
{
    if (!CFEqual (requestedTypeUUID, kAudioServerPlugInTypeUUID))
        return nullptr;

    if (gDriver == nullptr)
        gDriver = new GLADriver();

    return gDriver->GetReference();
}
