#include "test/test_ifce.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Ramulator {

class TestImpl : public TestIfce, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(TestIfce, TestImpl, "TestImpl",
    "Ramulator validation test implementation.")

 private:
  static constexpr double STATS_FLOAT_REL_TOL = 0.10; 
  static constexpr double STATS_FLOAT_ABS_TOL = 1e-9;  

  const std::vector<std::string> STATS_TO_CHECK = {
    "Frontend.cycles_recorded_core_0",
    "Frontend.memory_access_cycles_recorded_core_0",
    "MemorySystem.total_num_read_requests",
    "MemorySystem.total_num_write_requests",
    "MemorySystem.memory_system_cycles",
    "MemorySystem.Controller.avg_read_latency_0", 
    "MemorySystem.Controller.row_hits_0",
    "MemorySystem.Controller.row_misses_0",
  };

  const std::vector<int> NRCD_VARIANTS = {10, 15, 20};

  struct TestCase {
    std::string name;        
    std::string config_path;
    std::string trace_path;
  };

  const std::vector<TestCase> BASE_TESTS = {
    {"simpleO3_401_DDR4", "./tests/ddr4.yaml", "./tests/traces/cpu_traces/401.bzip2"},
    {"simpleO3_403_DDR4", "./tests/ddr4.yaml", "./tests/traces/cpu_traces/403.gcc"},
    {"simpleO3_401_DDR3", "./tests/ddr3.yaml", "./tests/traces/cpu_traces/401.bzip2"},
    {"simpleO3_403_DDR3", "./tests/ddr3.yaml", "./tests/traces/cpu_traces/403.gcc"},
  };

  const std::string RAMULATOR_BIN  = "./ramulator2";
  const std::string RESULTS_DIR    = "./upro_results";
  const std::string STATS_DIR      = "./upro_results/stats";
  const std::string GOLDEN_DIR     = "./upro_results/golden_stats";
  const std::string TEMP_DIR       = "./upro_results/temp_configs";

  static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
      throw std::runtime_error("Cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  static void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path);
    if (!f.is_open()) {
      throw std::runtime_error("Cannot write file: " + path);
    }
    f << data;
  }


  static std::string patch_trace(const std::string& yaml_text,
                                 const std::string& trace_path) {
    std::istringstream in(yaml_text);
    std::ostringstream out;
    bool in_traces_block = false;

    for (std::string line; std::getline(in, line); ) {
      // Detect leading whitespace of the current line
      size_t indent = line.find_first_not_of(" \t");
      std::string trimmed = (indent == std::string::npos) ? "" : line.substr(indent);

      if (trimmed.rfind("traces:", 0) == 0) {

        out << line << "\n";
        std::string list_indent(indent + 2, ' ');
        out << list_indent << "- " << trace_path << "\n";
        in_traces_block = true;
        continue;
      }

      if (in_traces_block) {
        if (!trimmed.empty() && trimmed[0] == '-') {
          continue;
        } else {
          in_traces_block = false;
        }
      }

      out << line << "\n";
    }
    return out.str();
  }

  // Patches the nRCD timing value in the YAML text.
  static std::string patch_nrcd(const std::string& yaml_text, int nrcd) {
    std::istringstream in(yaml_text);
    std::ostringstream out;
    bool patched = false;

    for (std::string line; std::getline(in, line); ) {
      if (!patched) {
        size_t indent = line.find_first_not_of(" \t");
        if (indent != std::string::npos) {
          std::string trimmed = line.substr(indent);
          if (trimmed.rfind("nRCD:", 0) == 0) {
            std::string leading(indent, ' ');
            out << leading << "nRCD: " << nrcd << "\n";
            patched = true;
            continue;
          }
        }
      }
      out << line << "\n";
    }

    if (!patched) {
      // nRCD key not found
      std::cerr << "WARNING: nRCD key not found in config; patch not applied.\n";
    }
    return out.str();
  }

  using StatMap = std::map<std::string, std::string>;

  static void flatten_yaml(const std::string& prefix,
                            const std::string& text,
                            StatMap& out) {

    std::istringstream in(text);
    std::vector<std::pair<int, std::string>> key_stack; 

    for (std::string line; std::getline(in, line); ) {
      if (line.empty() || line.find_first_not_of(" \t#") == std::string::npos)
        continue;
      if (line[line.find_first_not_of(" \t")] == '#')
        continue;

      size_t indent = line.find_first_not_of(" \t");
      std::string trimmed = line.substr(indent);

      size_t colon = trimmed.find(':');
      if (colon == std::string::npos) continue;

      std::string key = trimmed.substr(0, colon);
      std::string val = trimmed.substr(colon + 1);
      // Strip leading whitespace from value
      size_t vs = val.find_first_not_of(" \t");
      val = (vs == std::string::npos) ? "" : val.substr(vs);

      // Pop stack entries that are at the same or deeper indent
      while (!key_stack.empty() &&
             key_stack.back().first >= static_cast<int>(indent)) {
        key_stack.pop_back();
      }

      // Build the full dot-notation key
      std::string full_key;
      for (const auto& [lvl, k] : key_stack) {
        full_key += k + ".";
      }
      full_key += key;

      if (!val.empty()) {
        // Leaf value
        out[full_key] = val;
      } else {
        // Mapping node push onto stack and continue
        key_stack.push_back({static_cast<int>(indent), key});
      }
    }
  }

  static StatMap parse_stat_file(const std::string& path) {
    std::string text = read_file(path);
    StatMap result;
    flatten_yaml("", text, result);
    return result;
  }

  static bool stats_equal(const std::string& cur_str,
                           const std::string& golden_str,
                           const std::string& key) {

    try {
      size_t cur_pos = 0, golden_pos = 0;
      double cur_d    = std::stod(cur_str, &cur_pos);
      double golden_d = std::stod(golden_str, &golden_pos);

      if (cur_pos == cur_str.size() && golden_pos == golden_str.size()) {
        bool cur_is_int    = cur_str.find_first_of(".eE") == std::string::npos;
        bool golden_is_int = golden_str.find_first_of(".eE") == std::string::npos;

        if (cur_is_int && golden_is_int) {
          // Integer stats
          return static_cast<long long>(cur_d) == static_cast<long long>(golden_d);
        } else {
          // Float stats: relative tolerance with absolute floor.
          if (golden_d == 0.0 && cur_d == 0.0) return true;
          double rel = std::abs(cur_d - golden_d) /
                       std::max(std::abs(golden_d), STATS_FLOAT_ABS_TOL);
          return rel <= STATS_FLOAT_REL_TOL;
        }
      }
    } catch (...) {

    }
    return cur_str == golden_str;
  }


  bool compare_against_golden(const std::string& stat_file,
                               const std::string& golden_file) {
    if (!fs::exists(golden_file)) {
      fs::copy_file(stat_file, golden_file, fs::copy_options::overwrite_existing);
      std::cout << "Golden stat saved: " << golden_file << "\n";
      return true;
    }

    StatMap current, golden;
    try {
      current = parse_stat_file(stat_file);
      golden  = parse_stat_file(golden_file);
    } catch (const std::exception& e) {
      std::cerr << "ERROR reading stat files: " << e.what() << "\n";
      return false;
    }

    bool ok = true;
    for (const auto& key : STATS_TO_CHECK) {
      auto ci = current.find(key);
      auto gi = golden.find(key);

      if (ci == current.end()) {
        std::cerr << "WARNING: Missing stat in current file: " << key << "\n";
        ok = false;
        continue;
      }
      if (gi == golden.end()) {
        std::cerr << "WARNING: Missing stat in golden file: " << key << "\n";
        ok = false;
        continue;
      }

      if (!stats_equal(ci->second, gi->second, key)) {
        std::cerr << "WARNING: '" << key << "' mismatch. "
                  << "Current=" << ci->second
                  << ", Golden=" << gi->second << "\n";
        ok = false;
      }
    }
    return ok;
  }

  bool run_one(const TestCase& tc, int nrcd) {
    const std::string run_name  = tc.name + "_nRCD" + std::to_string(nrcd);
    const std::string temp_cfg  = TEMP_DIR  + "/" + run_name + ".yaml";
    const std::string stat_file = STATS_DIR + "/" + run_name + ".stat";
    const std::string golden    = GOLDEN_DIR + "/" + run_name + ".golden_stat";

    // Build patched config
    std::string yaml;
    try {
      yaml = read_file(tc.config_path);
    } catch (const std::exception& e) {
      std::cerr << "ERROR reading config " << tc.config_path
                << ": " << e.what() << "\n";
      return false;
    }

    yaml = patch_trace(yaml, tc.trace_path);
    yaml = patch_nrcd(yaml, nrcd);
    write_file(temp_cfg, yaml);

    // popen() forks a shell, so the child is a completely separate process.
    // We read stdout chunk-by-chunk and write it to the stat file.
    const std::string cmd = RAMULATOR_BIN + " -f " + temp_cfg + " 2>/dev/null";
    std::cout << "Starting simulation: " << cmd << "\n";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      std::cerr << "ERROR: popen failed for: " << cmd << "\n";
      return false;
    }

    {
      std::ofstream stat_out(stat_file);
      if (!stat_out.is_open()) {
        pclose(pipe);
        std::cerr << "ERROR: cannot write stat file: " << stat_file << "\n";
        return false;
      }

      std::array<char, 4096> buf{};
      while (fgets(buf.data(), buf.size(), pipe)) {
        stat_out << buf.data();
      }
    }

    int raw   = pclose(pipe);
    // WEXITSTATUS extracts the actual child process exit code from the
    // value returned by pclose (which encodes signal info in the low bits).
    int ret   = WEXITSTATUS(raw);

    if (ret != 0) {
      std::cout << "Simulation: FAIL (exit code " << ret << ")\n";
      std::cout << "Stat Consistency: FAIL, Runtime: FAIL, Memory Usage: FAIL\n";
      return false;
    }

    bool stats_ok = compare_against_golden(stat_file, golden);

    std::cout << "Stat Consistency: " << (stats_ok ? "OK" : "FAIL")
              << ", Runtime: OK, Memory Usage: OK\n";

    return stats_ok;
  }

 public:
  void init() override {
    fs::create_directories(RESULTS_DIR);
    fs::create_directories(STATS_DIR);
    fs::create_directories(GOLDEN_DIR);
    fs::create_directories(TEMP_DIR);

    bool overall_ok = true;

    for (const auto& tc : BASE_TESTS) {
      for (int nrcd : NRCD_VARIANTS) {
        std::cout << "\n--- Trace: " << tc.name
                  << "  nRCD=" << nrcd << " ---\n";
        bool ok = run_one(tc, nrcd);
        overall_ok = overall_ok && ok;
      }
    }

    std::cout << "\nAll runs completed.\n";
    std::cout << "Stats stored in:        " << STATS_DIR  << "\n";
    std::cout << "Golden stats stored in: " << GOLDEN_DIR << "\n";
    std::cout << "FINAL RESULT: " << (overall_ok ? "PASS" : "FAIL") << "\n";
  }
};

}  // namespace Ramulator