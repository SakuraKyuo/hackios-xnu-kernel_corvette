#pragma once

#include <cstdint>

#include "hakios-metal/a6xx.hpp"

namespace hakios::metal::gpu_identity {

enum class Family : uint8_t {
    Unknown,
    A6xx,
    A7xx,
};

// The legacy MSM GPU_ID returned by the Corvette kernel's GET_PARAM(0x03).
inline constexpr uint32_t kCorvetteA750MsmGpuId = 0x43051401u;

// This is the Gen7 GMU/firmware chip value, not the MSM GPU_ID above.
inline constexpr uint32_t kCorvetteA750GmuChipId = 0x07090000u;

constexpr Family familyFromDriverId(uint32_t id) {
    if (id == a6xx::kChipId) return Family::A6xx;
    if (id == kCorvetteA750MsmGpuId) return Family::A7xx;
    return Family::Unknown;
}

// A7xx is identified for routing, but is not advertised until its packets,
// descriptors, and compiler path have been implemented and tested.
constexpr bool backendAvailable(uint32_t id) {
    return id == 0 || familyFromDriverId(id) == Family::A6xx;
}

static_assert(kCorvetteA750MsmGpuId != kCorvetteA750GmuChipId);

}  // namespace hakios::metal::gpu_identity
