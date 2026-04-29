#include "GLA_IPCTypes.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static void testChannelMapSerialize()
{
    std::vector<GLAChannelEntry> entries(2);
    entries[0].entityId = 0xDEADBEEF12345678ULL;
    strncpy(entries[0].displayName, "Bob's Guitar", sizeof(entries[0].displayName) - 1);

    entries[1].entityId = 0xCAFEBABE00000001ULL;
    strncpy(entries[1].displayName, "Alice's Vocals", sizeof(entries[1].displayName) - 1);

    auto buf = serializeChannelMapUpdate(entries);

    // Deserialise.
    assert(buf.size() >= 8);
    uint32_t type = 0, count = 0;
    memcpy(&type,  buf.data(),     4);
    memcpy(&count, buf.data() + 4, 4);

    assert(static_cast<GLAMsgType>(type) == GLAMsgType::ChannelMapUpdate);
    assert(count == 2);
    assert(buf.size() == 8 + 2 * sizeof(GLAChannelEntry));

    std::vector<GLAChannelEntry> decoded(count);
    memcpy(decoded.data(), buf.data() + 8, count * sizeof(GLAChannelEntry));

    assert(decoded[0].entityId == 0xDEADBEEF12345678ULL);
    assert(strcmp(decoded[0].displayName, "Bob's Guitar") == 0);
    assert(decoded[1].entityId == 0xCAFEBABE00000001ULL);
    assert(strcmp(decoded[1].displayName, "Alice's Vocals") == 0);

    printf("testChannelMapSerialize: PASS\n");
}

static void testEntityListSerialize()
{
    std::vector<GLAEntityInfo> entities(1);
    entities[0].entityId   = 0x1234567890ABCDEFULL;
    entities[0].online      = true;
    entities[0].streamCount = 2;
    strncpy(entities[0].name, "FoH Out", sizeof(entities[0].name) - 1);

    auto buf = serializeEntityList(entities);

    uint32_t type = 0, count = 0;
    memcpy(&type,  buf.data(),     4);
    memcpy(&count, buf.data() + 4, 4);

    assert(static_cast<GLAMsgType>(type) == GLAMsgType::EntityListResponse);
    assert(count == 1);

    std::vector<GLAEntityInfo> decoded(count);
    memcpy(decoded.data(), buf.data() + 8, count * sizeof(GLAEntityInfo));
    assert(decoded[0].entityId == 0x1234567890ABCDEFULL);
    assert(decoded[0].online == true);
    assert(strcmp(decoded[0].name, "FoH Out") == 0);

    printf("testEntityListSerialize: PASS\n");
}

int main()
{
    testChannelMapSerialize();
    testEntityListSerialize();
    printf("All IPC protocol tests passed.\n");
    return 0;
}
