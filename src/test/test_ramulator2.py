#!/usr/bin/env python3
import copy
import math
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import time
import psutil
import yaml
import colorama


MEM_USAGE_THRESHOLD_MIB = 200
RUNTIME_ERROR_MARGIN = 0.2
STATS_ERROR_MARGIN = 0.1

RAMULATOR_BIN = "./ramulator2"
BASE_CONFIG = "./src/test/ddr4.yaml"

UPRO_RESULTS = "./upro_results"
TEMP_CONFIGS = os.path.join(UPRO_RESULTS, "temp_configs")
STATS_DIR = os.path.join(UPRO_RESULTS, "stats")
GOLDEN_STATS_DIR = os.path.join(UPRO_RESULTS, "golden_stats")

TRACES = [
    {
        "name": "simpleO3_401",
        "trace_path": "./src/test/traces/cpu_traces/401.bzip2",
        "expected_runtime": 15,
    },
    {
        "name": "simpleO3_403",
        "trace_path": "./src/test/traces/cpu_traces/403.gcc",
        "expected_runtime": 7,
    },
]

# nRCD variants to sweep. Each value produces a separate config and golden-stat
# entry, giving the test coverage across timing configurations. nRCD controls the
# RAS-to-CAS delay:- the number of cycles between activating a row and issuing a
# column command. A tighter value stresses the timing path; a looser value shows
# how the controller absorbs additional latency into avg_read_latency and row_miss penalties.
NRCD_VARIANTS = [10, 15, 20]

# Metrics to compare against golden stats
STATS_TO_CHECK = [
    "Frontend.cycles_recorded_core_0",
    "Frontend.memory_access_cycles_recorded_core_0",
    "MemorySystem.total_num_read_requests",
    "MemorySystem.total_num_write_requests",
    "MemorySystem.memory_system_cycles",
    "MemorySystem.Controller.avg_read_latency_0",
    "MemorySystem.Controller.row_hits_0",
    "MemorySystem.Controller.row_misses_0",
]


def setup_dirs():
    os.makedirs(UPRO_RESULTS, exist_ok=True)
    os.makedirs(TEMP_CONFIGS, exist_ok=True)
    os.makedirs(STATS_DIR, exist_ok=True)
    os.makedirs(GOLDEN_STATS_DIR, exist_ok=True)


def flatten(prefix, obj, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            new_prefix = f"{prefix}.{k}" if prefix else str(k)
            flatten(new_prefix, v, out)
    elif isinstance(obj, list):
        out[prefix] = obj
    else:
        out[prefix] = obj


def load_stat_file(stat_path):
    with open(stat_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    if not isinstance(data, dict):
        raise ValueError(f"Invalid stat file format: {stat_path}")

    flat = {}
    flatten("", data, flat)
    return flat


def is_number(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def stats_equal(cur_val, golden_val):
    if is_number(cur_val) and is_number(golden_val):
        if isinstance(cur_val, float) or isinstance(golden_val, float):
            return math.isclose(
                cur_val,
                golden_val,
                rel_tol=STATS_ERROR_MARGIN,
                abs_tol=1e-9,
            )
        return cur_val == golden_val
    return cur_val == golden_val


def compare_against_golden(stats_filename):
    base = os.path.splitext(os.path.basename(stats_filename))[0]
    golden_stats_filename = os.path.join(GOLDEN_STATS_DIR, base + ".golden_stat")

    if not os.path.exists(golden_stats_filename):
        print(f"Saving current simulation result as golden: {golden_stats_filename}")
        shutil.copyfile(stats_filename, golden_stats_filename)
        return True

    current_stats = load_stat_file(stats_filename)
    golden_stats = load_stat_file(golden_stats_filename)

    mismatch = False

    for stat in STATS_TO_CHECK:
        if stat not in current_stats:
            print(f"WARNING: Missing stat in current file: {stat}")
            mismatch = True
            continue

        if stat not in golden_stats:
            print(f"WARNING: Missing stat in golden file: {stat}")
            mismatch = True
            continue

        cur_val = current_stats[stat]
        golden_val = golden_stats[stat]

        if not stats_equal(cur_val, golden_val):
            print(
                f"WARNING: '{stat}' mismatch. "
                f"Current={cur_val}, Golden={golden_val}"
            )
            mismatch = True

    return not mismatch

def run_sim(trace_info, nrcd, ok_str, fail_str, warn_str):
    base_name = trace_info["name"]
    trace_path = trace_info["trace_path"]
    expected_runtime = trace_info["expected_runtime"]

    run_name = f"{base_name}_nRCD{nrcd}"

    with open(BASE_CONFIG, "r", encoding="utf-8") as f:
        base_config = yaml.safe_load(f)

    config = copy.deepcopy(base_config)
    config["Frontend"]["impl"] = "SimpleO3"
    config["Frontend"]["traces"] = [trace_path]
    config["MemorySystem"]["DRAM"]["timing"]["nRCD"] = nrcd

    temp_cfg_path = os.path.join(TEMP_CONFIGS, f"{run_name}.yaml")
    with open(temp_cfg_path, "w", encoding="utf-8") as f:
        yaml.safe_dump(config, f, sort_keys=False)

    stats_filename = os.path.join(STATS_DIR, f"{run_name}.stat")
    args = [RAMULATOR_BIN, "-f", temp_cfg_path]

    print(f"Starting simulation: {' '.join(args)}")

    tmp_err = tempfile.NamedTemporaryFile(delete=False)
    tmp_err.close()
    start_time = time.time()

    stdout_queue = queue.Queue()

    stderr_fh = open(tmp_err.name, "w")
    try:
        p = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=stderr_fh,
            text=True,
        )
    finally:
        stderr_fh.close()

    def drain_stdout(pipe, q):
        for chunk in iter(lambda: pipe.read(4096), ""):
            q.put(chunk)
        pipe.close()

    drain_thread = threading.Thread(target=drain_stdout, args=(p.stdout, stdout_queue), daemon=True)
    drain_thread.start()

    proc = psutil.Process(p.pid)

    mem_usage_bytes = 0
    while p.poll() is None:
        try:
            mem_usage_bytes = max(mem_usage_bytes, proc.memory_info().rss)
        except Exception:
            pass
        time.sleep(0.1)

    drain_thread.join()
    stdout_data = "".join(stdout_queue.queue)

    execution_time_sec = time.time() - start_time
    mem_usage_mib = float(mem_usage_bytes) / 2**20

    with open(stats_filename, "w", encoding="utf-8") as f:
        f.write(stdout_data or "")

    with open(tmp_err.name, "r", encoding="utf-8") as f:
        stderr_data = f.read()
    os.unlink(tmp_err.name)

    if p.returncode != 0:
        print(f"Simulation: {fail_str}")
        if stderr_data.strip():
            print(stderr_data.strip())
        return False

    mem_usage_ok = True
    if mem_usage_mib > MEM_USAGE_THRESHOLD_MIB:
        print(
            f"{warn_str} Ramulator used {mem_usage_mib:.2f} MiB memory, "
            f"which is more than the threshold: {MEM_USAGE_THRESHOLD_MIB} MiB."
        )
        mem_usage_ok = False

    runtime_ok = True
    if expected_runtime <= 0:
        print(f"{warn_str} expected_runtime not set for this trace — skipping runtime check.")
    elif execution_time_sec > expected_runtime * (1 + RUNTIME_ERROR_MARGIN):
        print(
            f"{warn_str} Ramulator completed the simulation in {execution_time_sec:.2f} seconds, "
            f"which is more than {int(RUNTIME_ERROR_MARGIN * 100)}% higher than "
            f"the expected runtime: {expected_runtime} seconds."
        )
        runtime_ok = False

    stats_ok = compare_against_golden(stats_filename)

    print(
        f"Stat Consistency: {ok_str if stats_ok else fail_str}, "
        f"Runtime: {ok_str if runtime_ok else warn_str}, "
        f"Memory Usage: {ok_str if mem_usage_ok else warn_str}"
    )

    return stats_ok


def main():
    setup_dirs()
    colorama.init()

    ok_str = colorama.Fore.GREEN + "OK" + colorama.Style.RESET_ALL
    fail_str = colorama.Fore.RED + "FAIL" + colorama.Style.RESET_ALL
    warn_str = colorama.Fore.YELLOW + "WARNING:" + colorama.Style.RESET_ALL

    overall_ok = True

    for trace_info in TRACES:
        for nrcd in NRCD_VARIANTS:
            print(f"\n--- Trace: {trace_info['name']}  nRCD={nrcd} ---")
            result = run_sim(trace_info, nrcd, ok_str, fail_str, warn_str)
            overall_ok = overall_ok and result

    print("\nAll runs completed.")
    print("Current stats stored in:", STATS_DIR)
    print("Golden stats stored in:", GOLDEN_STATS_DIR)

    if overall_ok:
        print("FINAL RESULT:", ok_str)
    else:
        print("FINAL RESULT:", fail_str)


if __name__ == "__main__":
    main()