#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace mw::core
{
    using StableId = std::uint64_t;

    inline StableId allocateStableId() noexcept
    {
        static std::atomic<StableId> nextId {
            []
            {
                const auto now = static_cast<StableId>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                return (now & (std::numeric_limits<StableId>::max() >> 2U)) + 1U;
            }()
        };

        auto id = nextId.fetch_add(1U, std::memory_order_relaxed);
        if (id == 0U)
            id = nextId.fetch_add(1U, std::memory_order_relaxed);
        return id;
    }
}
