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

// Mesa's fd_dev_id for these MSM GPU_ID values. A zero GPU_ID keeps the
// existing A660 fallback; the GMU firmware ID is deliberately not accepted.
struct Ir3Target {
    uint32_t gpuId;
    uint64_t chipId;
    bool known;
};

constexpr Ir3Target ir3TargetForMsmGpuId(uint32_t id) {
    if (id == 0 || id == a6xx::kChipId) return {0, a6xx::kChipId, true};
    if (id == kCorvetteA750MsmGpuId) return {0, kCorvetteA750MsmGpuId, true};
    return {0, 0, false};
}

// A7xx is identified for routing, but is not advertised until its packets,
// descriptors, and kernel backend have been implemented and tested.
constexpr bool backendAvailable(uint32_t id) {
    return id == 0 || familyFromDriverId(id) == Family::A6xx;
}

static_assert(kCorvetteA750MsmGpuId != kCorvetteA750GmuChipId);

}  // namespace hakios::metal::gpu_identity
