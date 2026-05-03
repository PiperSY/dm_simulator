#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "experiments/runner.hpp"

namespace {

std::filesystem::path write_runner_config(const std::filesystem::path& path,
                                          const std::filesystem::path& output_dir) {
    std::ofstream output(path);
    output << "experiment:\n"
           << "  name: runner_test\n"
           << "  output_dir: " << output_dir.string() << "\n"
           << "\n"
           << "memory:\n"
           << "  node_id: 99\n"
           << "  base_latency: 5\n"
           << "  bandwidth_bytes_per_time: 8\n"
           << "\n"
           << "link:\n"
           << "  one_way_latency: 2\n"
           << "\n"
           << "local_cache:\n"
           << "  capacity_bytes: 64\n"
           << "  hit_latency: 1\n"
           << "  policy: lru\n"
           << "\n"
           << "workload:\n"
           << "  seed: 99\n"
           << "  compute_node_ids: [1, 2]\n"
           << "  object_count: 16\n"
           << "  object_size_bytes: 8\n"
           << "  requests_per_node_per_epoch: 2\n"
           << "  epoch_count: 1\n"
           << "  hot_set_size: 1\n"
           << "  hot_access_probability: 1.0\n"
           << "  hot_set_mode: static\n"
           << "  cross_node_overlap: high\n";
    return path;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::size_t line_count(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    return count;
}

void test_runner_writes_expected_outputs() {
    const std::filesystem::path base_dir =
        std::filesystem::temp_directory_path() / "dm_sim_runner_test";
    const std::filesystem::path output_dir = base_dir / "results";
    const std::filesystem::path config_path = base_dir / "runner.yaml";
    std::filesystem::create_directories(base_dir);

    write_runner_config(config_path, output_dir);

    const dm_sim::ExperimentRunner runner;
    const dm_sim::ExperimentResult result =
        runner.run_config(config_path.string(), output_dir.string());

    assert(result.summary.experiment_name == "runner_test");
    assert(result.summary.completed_requests == 4);
    assert(result.summary.per_node.size() == 2);
    assert(std::filesystem::exists(output_dir / "summary.json"));
    assert(std::filesystem::exists(output_dir / "per_node.csv"));
    assert(std::filesystem::exists(output_dir / "latencies.csv"));

    const std::string summary_json = read_file(output_dir / "summary.json");
    assert(summary_json.find("\"experiment_name\": \"runner_test\"") !=
           std::string::npos);
    assert(summary_json.find("\"completed_requests\": 4") != std::string::npos);
    assert(summary_json.find("\"latency\"") != std::string::npos);
    assert(summary_json.find("\"per_node\"") != std::string::npos);

    assert(line_count(output_dir / "per_node.csv") == 3);
    assert(line_count(output_dir / "latencies.csv") == 5);
}

}  // namespace

int main() {
    test_runner_writes_expected_outputs();
    return 0;
}
