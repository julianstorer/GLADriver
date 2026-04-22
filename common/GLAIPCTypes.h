#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Wire format: 4-byte LE message type + 4-byte LE payload length + payload.

static constexpr uint32_t glaIpcMagic = 0x474C4100; // 'GLA\0'

enum class GLAMsgType : uint32_t
{
    // App -> clients (broadcast)
    ChannelMapUpdate    = 1,
    // clients -> App
    GetStatus           = 10,
    GetEntityList       = 11,
    SetRouting          = 12,
    SetNetworkInterface = 13,
    SetUSBBridge        = 14,
    // App -> clients
    StatusResponse      = 20,
    EntityListResponse  = 21,
    RoutingChanged      = 22,
};

struct GLAChannelEntry
{
    uint8_t  channelIndex;
    uint64_t entityId;
    char     displayName[64]; // UTF-8, null-terminated
};

struct GLAEntityInfo
{
    uint64_t entityId;
    char     name[64];
    uint8_t  streamCount;
    bool     online;
};

//==============================================================================
// Variable-length message helpers — serialise to/from a byte vector.
// Header: [type:u32 LE][count:u32 LE][entries...]

inline std::vector<uint8_t> serializeChannelMapUpdate (const std::vector<GLAChannelEntry>& entries)
{
    uint32_t type  = static_cast<uint32_t> (GLAMsgType::ChannelMapUpdate);
    uint32_t count = static_cast<uint32_t> (entries.size());
    std::vector<uint8_t> buf (8 + count * sizeof (GLAChannelEntry));
    memcpy (buf.data(),     &type,  4);
    memcpy (buf.data() + 4, &count, 4);
    memcpy (buf.data() + 8, entries.data(), count * sizeof (GLAChannelEntry));
    return buf;
}

inline std::vector<uint8_t> serializeEntityList (const std::vector<GLAEntityInfo>& entities)
{
    uint32_t type  = static_cast<uint32_t> (GLAMsgType::EntityListResponse);
    uint32_t count = static_cast<uint32_t> (entities.size());
    std::vector<uint8_t> buf (8 + count * sizeof (GLAEntityInfo));
    memcpy (buf.data(),     &type,  4);
    memcpy (buf.data() + 4, &count, 4);
    memcpy (buf.data() + 8, entities.data(), count * sizeof (GLAEntityInfo));
    return buf;
}
