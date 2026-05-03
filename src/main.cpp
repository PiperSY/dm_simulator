#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "experiments/runner.hpp"

namespace {

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name
              << " --config <path> [--output-dir <path>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string config_path;
    std::optional<std::string> output_dir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "--config requires a path\n";
                return 1;
            }
            config_path = argv[++i];
            continue;
        }
        if (arg == "--output-dir") {
            if (i + 1 >= argc) {
                std::cerr << "--output-dir requires a path\n";
                return 1;
            }
            output_dir = argv[++i];
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (config_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const dm_sim::ExperimentRunner runner;
        const dm_sim::ExperimentResult result =
            runner.run_config(config_path, output_dir);

        std::cout << "Experiment: " << result.summary.experiment_name << "\n";
        std::cout << "Output directory: " << result.output_dir << "\n";
        std::cout << "Completed requests: "
                  << result.summary.completed_requests << "\n";
        std::cout << "Mean latency: " << result.summary.mean_latency << "\n";
        std::cout << "P95 latency: " << result.summary.p95_latency << "\n";
        std::cout << "Local cache hit rate: "
                  << result.summary.local_cache_hit_rate << "\n";
    } catch (const std::exception& error) {
        std::cerr << "Experiment failed: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
