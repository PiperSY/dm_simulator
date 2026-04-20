#include <iostream>

#include "sim/config.hpp"
#include "sim/simulator.hpp"

int main() {
    const dm_sim::SimulationConfig config{
        1,
        2,
        5,
        20,
        16,
        {
            {1001, 64},
            {1002, 32},
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    std::cout << "Completed requests: "
              << simulator.stats().completed_requests() << "\n";
    std::cout << "Average latency: " << simulator.stats().average_latency() << "\n";

    return 0;
}
