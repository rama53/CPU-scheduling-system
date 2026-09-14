# CPU Scheduling and Deadlock Management Simulator

A C++ operating systems project that simulates CPU scheduling, process execution, I/O operations, resource allocation, and deadlock detection and recovery.

## Key Features

* Preemptive priority scheduling with round-robin time slicing.
* Priority aging to reduce process starvation.
* Process state management, including CPU execution, I/O blocking, and resource blocking.
* Resource allocation and release.
* Deadlock detection using a Banker's algorithm-based safety check.
* Deadlock recovery through process termination and resource release.
* Gantt chart generation and process performance metrics.

## Technologies

C++ | Operating Systems | CPU Scheduling | Deadlock Detection | File Handling

## Getting Started

### Compile

```bash
g++ -std=c++17 main.cpp -o scheduler
```

### Run

```bash
./scheduler
```

The program reads process and resource configurations from `input.txt` and displays the simulation results, Gantt chart, and performance metrics.

## Project Structure

```text
├── main.cpp
├── input.txt
└── README.md
```
