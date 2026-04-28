#include <iostream>

#include "sim/config.hpp"
#include "sim/simulator.hpp"

int main() {
    const dm_sim::SimulationConfig config{
        {
            {
                1,
                {
                    {1001, 64},
                },
            },
            {
                2,
                {
                    {1002, 32},
                },
            },
        },
        99,
        5,
        20,
        16,
        {
            64,
            1,
            dm_sim::LocalCachePolicyType::Lru,
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    std::cout << "Completed requests: "
              << simulator.stats().completed_requests() << "\n";
    std::cout << "Average latency: " << simulator.stats().average_latency() << "\n";
    std::cout << "Memory average wait: "
              << simulator.stats().average_memory_wait() << "\n";
    std::cout << "Local cache hit rate: "
              << simulator.stats().local_cache_hit_rate() << "\n";

    return 0;
}
