#include "gla_io_handler.hpp"
#include "gla_device.hpp"
#include <cstring>

GLAIOHandler::GLAIOHandler(GLAEntityDevice* device)
    : _device(device) {}

void GLAIOHandler::OnReadClientInput(const std::shared_ptr<aspl::Client>& /*client*/,
                                      const std::shared_ptr<aspl::Stream>& /*stream*/,
                                      Float64 /*zeroTimestamp*/,
                                      Float64 /*timestamp*/,
                                      void* bytes,
                                      UInt32 bytesCount)
{
    UInt32 frames = bytesCount / sizeof(float);
    _device->ringBuffer().read(static_cast<float*>(bytes), frames);
}
