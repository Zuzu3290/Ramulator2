# Ramulator 2 Validation Workflow

## Overview

This project implements a validation workflow for Ramulator 2, inspired by the testing methodology used in Ramulator 1.

**Goals:**

- Run multiple memory configurations and traces
- Collect simulation outputs
- Compare results against reference (golden) values
- Provide clear pass/fail validation outcomes

> Due to challenges integrating directly into the C++ interface/implementation framework of Ramulator 2, the validation logic was implemented as a standalone Python-based test harness, which successfully reproduces the intended validation behavior.

---

## What This Project Does

The Python test script performs the following:

1. Loads a base YAML configuration (DDR4)
2. Sweeps `nRCD` timing values (e.g., 10, 15, 20)
3. Runs multiple CPU traces:
   - `401.bzip2`
   - `403.gcc`
4. Executes Ramulator 2 for each configuration
5. Captures simulation output (`stdout`)
6. Stores results as `.stat` files
7. Compares results against `.golden_stat` reference files
8. Reports:
   - Stat consistency
   - Runtime deviation
   - Memory usage
   - Final `PASS` / `FAIL`

---

## Usage

The application is already built — cloning the repository allows you to directly run the test script:

```bash
python test_ramulator2.py
```

---

## First Run Behavior

**On the first run:**

- No `.golden_stat` files exist
- The script will automatically create them from current outputs

**Subsequent runs:**

- Results are compared against these golden references

---

## Limitations & Observations

- **DDR3** configurations caused runtime errors (`std::out_of_range`) and were not fully validated
- **Synthetic traces** (random/stream) were incompatible with the tested frontend configurations
- Validation was therefore performed using:
  - DDR4
  - CPU traces with `SimpleO3`