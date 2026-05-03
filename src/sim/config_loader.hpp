#pragma once

#include <string>

#include "sim/config.hpp"

namespace dm_sim {

/*********************************** 
 * ExperimentConfig struct encapsulates the overall configuration for a simulation experiment, including the experiment name, output directory, and the detailed simulation configuration. 
 * The load_experiment_config function reads an experiment configuration from a YAML file and constructs an ExperimentConfig instance, 
 * while the load_simulation_config function extracts just the SimulationConfig from the experiment configuration.
 ***********************************/
struct ExperimentConfig {
    std::string name;
    std::string output_dir;
    SimulationConfig simulation;
};

[[nodiscard]] ExperimentConfig load_experiment_config(const std::string& path);
[[nodiscard]] SimulationConfig load_simulation_config(const std::string& path);

}  // namespace dm_sim
