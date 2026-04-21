#pragma once
#include <aspl/Driver.hpp>
#include <memory>
#include <vector>
#include "../common/gla_ipc_types.hpp"

class GLAUSBReader;
class GLAUnifiedDevice;
class GLAIPCClient;

//==============================================================================
class GLADriver : public aspl::Driver
{
public:
    GLADriver();
    ~GLADriver() override;

protected:
    OSStatus Initialize() override;

    void applyChannelMap (const std::vector<GLAChannelEntry>& entries);

private:
    std::shared_ptr<GLAUSBReader>     usbReader;
    std::shared_ptr<GLAIPCClient>     ipcClient;
    std::shared_ptr<GLAUnifiedDevice> unifiedDevice;
};
