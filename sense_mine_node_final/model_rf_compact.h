#pragma once
#include <stdint.h>
namespace rf_compact {
constexpr uint32_t kNumFeatures = 23;
constexpr uint32_t kNumClasses = 6;
constexpr uint32_t kNumTrees = 10;
constexpr uint32_t kNumNodes = 73768;
struct __attribute__((packed)) Node {
    uint8_t feature;
    float threshold;
    uint32_t left_child;
    uint32_t right_child;
    uint8_t leaf_class;
};
extern const Node kNodes[kNumNodes];
extern const uint32_t kTreeRoots[kNumTrees];
extern const char* const kClassNames[kNumClasses];
int predict(const float* features, uint8_t* predicted_class, uint16_t* votes_out);
}
