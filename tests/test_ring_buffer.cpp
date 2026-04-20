#include "gla_ring_buffer.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>

static void testBasicReadWrite() {
    GLARingBuffer rb(16);

    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    assert(rb.write(in, 4) == 4);

    float out[4] = {};
    rb.read(out, 4);
    for (int i = 0; i < 4; ++i)
        assert(std::fabs(out[i] - in[i]) < 1e-6f);

    printf("testBasicReadWrite: PASS\n");
}

static void testUnderrun() {
    GLARingBuffer rb(16);

    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.write(in, 4);

    float out[8] = {};
    rb.read(out, 8);

    // First 4 samples should match, next 4 should be zero (underrun).
    for (int i = 0; i < 4; ++i)
        assert(std::fabs(out[i] - in[i]) < 1e-6f);
    for (int i = 4; i < 8; ++i)
        assert(std::fabs(out[i]) < 1e-6f);

    printf("testUnderrun: PASS\n");
}

static void testOverrun() {
    GLARingBuffer rb(8);

    float in[6] = {1, 2, 3, 4, 5, 6};
    assert(rb.write(in, 6) == 6);

    // Try to write 4 more; only 2 slots remain (capacity=8, 6 used, but capacity-used=2).
    float in2[4] = {7, 8, 9, 10};
    size_t written = rb.write(in2, 4);
    assert(written == 2);

    printf("testOverrun: PASS\n");
}

static void testConcurrent() {
    GLARingBuffer rb(4096);

    std::thread producer([&]{
        float sample = 0.0f;
        for (int i = 0; i < 10000; ++i) {
            float buf[16];
            for (auto& s : buf) s = ++sample;
            while (rb.write(buf, 16) == 0)
                std::this_thread::yield();
        }
    });

    std::thread consumer([&]{
        float prev = 0.0f;
        int consumed = 0;
        while (consumed < 160000) {
            float buf[16] = {};
            rb.read(buf, 16);
            for (auto s : buf) {
                if (s != 0.0f) {
                    assert(s > prev);
                    prev = s;
                }
            }
            consumed += 16;
        }
    });

    producer.join();
    consumer.join();
    printf("testConcurrent: PASS\n");
}

int main() {
    testBasicReadWrite();
    testUnderrun();
    testOverrun();
    testConcurrent();
    printf("All ring buffer tests passed.\n");
    return 0;
}
