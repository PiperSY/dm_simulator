#pragma once

#include "workloads/workload.hpp"

namespace dm_sim {

// Builds deterministic synthetic request streams from the supplied config.
//
// The generated workload models per-node hot sets, optional epoch-to-epoch
// hot-set movement, and configurable overlap between compute nodes.
[[nodiscard]] GeneratedWorkload generate_synthetic_workload(
    const SyntheticWorkloadConfig& config);

}  // namespace dm_sim
