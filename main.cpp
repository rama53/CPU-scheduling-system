#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <regex>
#include <deque>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using namespace std;

// Resource structure holding resource ID and total available instances
struct Resource
{
    int id;
    int instances;
};

struct InstructionStep
{
    char type;              // determine if the instruction is a time or resource release or allocation
    int time_value;         // time value for resource allocation or release
    int resource_id;        // resource id for allocation and release
    int resource_count;     // number of resources
};

struct ProcessBurst
{
    string type;            // "CPU" or "IO"
    int io_duration = 0;
    vector<InstructionStep> steps;
};

enum ProcessState
{
    NEW,
    READY,
    RUNNING,
    BLOCKED_IO,
    BLOCKED_RESOURCE,
    FINISHED,
    TERMINATED
};

struct Process
{
    int process_id = 0;
    int arrival_time = 0;
    int base_priority = 0;      // original priority
    int current_priority = 0;   // priority after aging

    string bursts_text;
    vector<ProcessBurst> bursts;

    int current_burst_index = 0;           // current burst index
    int current_step_index = 0;            // current instruction in CPU burst
    int remaining_cpu_time = 0;            // remaining time in CPU burst
    int remaining_io_time = 0;             // remaining IO time

    int io_start_time = -1;                // time when IO begins consuming units

    int round_robin_used = 0;              // round-robin used in current CPU slice
    int ready_wait_ticks = 0;              // counts waited ticks toward aging
    long long total_waiting_time = 0;

    int ready_enter_time = -1;             // time entered ready
    int finish_time = -1;

    long long ready_sequence = 0;

    vector<int> allocated_resources;       // allocated resources
    int waiting_resource_index = -1;       // waiting resource index
    int waiting_resource_count = 0;        // waiting count

    ProcessState state = NEW;
    bool is_done() const
    {
        return state == FINISHED || state == TERMINATED;
    }
};

static const int PRIORITY_MIN = 0;  //defining priorities range [0-20]
static const int PRIORITY_MAX = 20;

static const int TIME_QUANTUM = 30;
static const int AGING_TICK_LIMIT = 10;

deque<int> ready_queues[PRIORITY_MAX + 1];  //implementing ready queue
long long ready_sequence_counter = 0;

static void trim_whitespace(string &text)
{
    int start = 0, end = (int)text.size() - 1;
    while (start <= end && isspace((unsigned char)text[start])) start++;
    while (end >= start && isspace((unsigned char)text[end])) end--;
    text = (start <= end) ? text.substr(start, end - start + 1) : "";
}

static bool is_all_digits(const string &text)
{
    if (text.empty()) return false;
    for (char c : text) if (!isdigit((unsigned char)c)) return false;
    return true;
}

static vector<string> split_cpu_items(const string &content)
{
    vector<string> result;
    string current;
    int bracket_depth = 0;
    for (char c : content)
    {
        if (c == '[') bracket_depth++;
        if (c == ']') bracket_depth--;
        if (c == ',' && bracket_depth == 0)
        {
            trim_whitespace(current);
            if (!current.empty()) result.push_back(current);
            current.clear();
        }
        else current.push_back(c);
    }
    trim_whitespace(current);
    if (!current.empty()) result.push_back(current);
    return result;
}

static InstructionStep parse_step_token(string token)
{
    trim_whitespace(token);
    InstructionStep step{};
    step.time_value = step.resource_id = step.resource_count = 0;

    if (is_all_digits(token))
    {
        step.type = 'T';
        step.time_value = stoi(token);
        return step;
    }

    if (token.size() >= 6 && (token[0] == 'R' || token[0] == 'F') &&
            token[1] == '[' && token.back() == ']')
    {
        step.type = token[0];
        string inside = token.substr(2, token.size() - 3);
        trim_whitespace(inside);
        size_t comma_pos = inside.find(',');
        if (comma_pos == string::npos) throw runtime_error("Bad token: " + token);

        string first_part = inside.substr(0, comma_pos);
        string second_part = inside.substr(comma_pos + 1);
        trim_whitespace(first_part);
        trim_whitespace(second_part);
        if (!is_all_digits(first_part) || !is_all_digits(second_part))
            throw runtime_error("Bad token: " + token);

        step.resource_id = stoi(first_part);
        step.resource_count = stoi(second_part);
        return step;
    }

    throw runtime_error("Unknown token: " + token);
}

vector<Resource> parse_resources_line(const string &line)  //parsing resources to know instances and types
{
    vector<Resource> resources;
    regex pattern("\\[(\\d+)\\s*,\\s*(\\d+)\\]");
    smatch match;
    string text = line;
    while (regex_search(text, match, pattern))
    {
        resources.push_back({stoi(match[1]), stoi(match[2])});
        text = match.suffix();
    }
    return resources;
}

bool parse_process_line(const string &line, Process &process)  //parsing the process lines and info
{
    stringstream stream(line);
    int priority;
    if (!(stream >> process.process_id >> process.arrival_time >> priority)) return false;
    process.base_priority = priority;
    process.current_priority = priority;

    string remaining_text;
    getline(stream, remaining_text);
    while (!remaining_text.empty() && remaining_text[0] == ' ')
        remaining_text.erase(remaining_text.begin());
    process.bursts_text = remaining_text;
    return true;
}

vector<ProcessBurst> parse_bursts(const string &text)   //parsing the bursts (IO or CPU)
{
    vector<ProcessBurst> bursts;
    regex block_pattern(R"((CPU|IO)\s*\{([^}]*)\})");
    auto begin = sregex_iterator(text.begin(), text.end(), block_pattern);
    auto end = sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        string burst_type = (*it)[1].str();
        string burst_content = (*it)[2].str();
        trim_whitespace(burst_content);

        if (burst_type == "IO")
        {
            if (!is_all_digits(burst_content))
                throw runtime_error("IO burst must be number: " + burst_content);
            ProcessBurst burst;
            burst.type = "IO";
            burst.io_duration = stoi(burst_content);
            bursts.push_back(burst);
        }
        else
        {
            ProcessBurst burst;
            burst.type = "CPU";
            auto instruction_tokens = split_cpu_items(burst_content);
            for (auto &token : instruction_tokens)
                burst.steps.push_back(parse_step_token(token));
            bursts.push_back(burst);
        }
    }
    return bursts;
}

static const char* get_state_name(ProcessState state)
{
    switch (state)
    {
    case NEW:
        return "NEW";
    case READY:
        return "READY";
    case RUNNING:
        return "RUNNING";
    case BLOCKED_IO:
        return "BLOCKED_IO";
    case BLOCKED_RESOURCE:
        return "BLOCKED_RESOURCE";
    case FINISHED:
        return "FINISHED";
    case TERMINATED:
        return "TERMINATED";
    default:
        return "UNKNOWN";
    }
}

int find_best_ready_priority()
{
    for (int priority = PRIORITY_MIN; priority <= PRIORITY_MAX; priority++)
        if (!ready_queues[priority].empty()) return priority;
    return -1;
}

bool has_any_ready_process()
{
    for (int priority = PRIORITY_MIN; priority <= PRIORITY_MAX; priority++)
        if (!ready_queues[priority].empty()) return true;
    return false;
}

bool has_any_io_waiting_process(const vector<Process>& processes)
{
    for (auto &process : processes)
        if (process.state == BLOCKED_IO) return true;
    return false;
}

bool all_done_or_blocked_on_resource(const vector<Process>& processes)
{
    for (auto &process : processes)
    {
        if (process.is_done()) continue;
        if (process.state == BLOCKED_RESOURCE) continue;
        return false;
    }
    return true;
}

void rebuild_ready_queues(vector<Process> &processes)
{
    vector<int> ready_indices;
    for (int i = 0; i < (int)processes.size(); i++)
        if (processes[i].state == READY) ready_indices.push_back(i);

    for (int priority = PRIORITY_MIN; priority <= PRIORITY_MAX; priority++)
        ready_queues[priority].clear();

    sort(ready_indices.begin(), ready_indices.end(), [&](int idx_a, int idx_b)
    {
        if (processes[idx_a].current_priority != processes[idx_b].current_priority)
            return processes[idx_a].current_priority < processes[idx_b].current_priority;
        return processes[idx_a].ready_sequence < processes[idx_b].ready_sequence;
    });

    for (int index : ready_indices)
    {
        int priority = max(PRIORITY_MIN, min(PRIORITY_MAX, processes[index].current_priority));
        ready_queues[priority].push_back(index);
    }
}

// function to use the aged priority in ready queue and cpu
void push_ready_keep_aged_priority(vector<Process> &processes, int process_index, int current_time)
{
    Process &process = processes[process_index];
    process.state = READY;
    process.ready_wait_ticks = 0;
    process.round_robin_used = 0;
    process.ready_sequence = ready_sequence_counter++;
    process.ready_enter_time = current_time;

    int priority = max(PRIORITY_MIN, min(PRIORITY_MAX, process.current_priority));
    ready_queues[priority].push_back(process_index);
}

// function to use the base priority when returning
void push_ready_reset_to_base_priority(vector<Process> &processes, int process_index, int current_time)
{
    Process &process = processes[process_index];
    process.state = READY;

    process.current_priority = process.base_priority;

    process.ready_wait_ticks = 0;
    process.round_robin_used = 0;
    process.ready_sequence = ready_sequence_counter++;
    process.ready_enter_time = current_time;

    int priority = max(PRIORITY_MIN, min(PRIORITY_MAX, process.current_priority));
    ready_queues[priority].push_back(process_index);
}
//pop the highest priority process
int pop_ready_process(vector<Process> &processes, int current_time)
{
    for (int priority = PRIORITY_MIN; priority <= PRIORITY_MAX; priority++)
    {
        if (!ready_queues[priority].empty())
        {
            int process_index = ready_queues[priority].front();
            ready_queues[priority].pop_front();
            Process &process = processes[process_index];

            if (process.ready_enter_time != -1)
            {
                process.total_waiting_time += (current_time - process.ready_enter_time);
                process.ready_enter_time = -1;
            }
            process.state = RUNNING;

            process.round_robin_used = 0;
            process.ready_wait_ticks = 0;
            return process_index;
        }
    }
    return -1;
}

vector<int> compute_available_resources(const vector<int>& total_resources,
                                        const vector<Process>& processes)
{
    vector<int> available = total_resources;
    for (auto &process : processes)
    {
        if (process.state == TERMINATED || process.state == FINISHED) continue;
        for (int k = 0; k < (int)available.size(); k++)
            available[k] -= process.allocated_resources[k];
    }
    return available;
}

static void set_blocked_on_resource(Process &process, int resource_index, int resource_count)
{
    process.waiting_resource_index = resource_index;
    process.waiting_resource_count = resource_count;
    process.state = BLOCKED_RESOURCE;
}

bool try_unblock_resource_waiters(vector<Process>& processes,const vector<int>& total_resources,const vector<Resource>& resources,int current_time)
{
    bool any_unblocked = false;
    bool made_progress = true;

    while (made_progress)
    {
        made_progress = false;

        for (int i = 0; i < (int)processes.size(); i++)
        {
            Process &process = processes[i];
            if (process.state != BLOCKED_RESOURCE) continue;
            if (process.is_done()) continue;

            int resource_index = process.waiting_resource_index;
            int needed_count = process.waiting_resource_count;
            if (resource_index < 0 || needed_count <= 0) continue;

            vector<int> available = compute_available_resources(total_resources, processes);

            if (available[resource_index] >= needed_count)
            {
                process.allocated_resources[resource_index] += needed_count;
                process.waiting_resource_index = -1;
                process.waiting_resource_count = 0;

                if (process.current_burst_index < (int)process.bursts.size() &&
                        process.bursts[process.current_burst_index].type == "CPU")
                {
                    process.current_step_index++;
                }

                push_ready_keep_aged_priority(processes, i, current_time);

                any_unblocked = true;
                made_progress = true;

                int display_resource_id = (resource_index < (int)resources.size())
                                          ? resources[resource_index].id : resource_index;
                cout << "  [Time " << current_time << "] Unblocked P" << process.process_id
                     << " granted R" << display_resource_id << "[" << needed_count << "]\n";
            }
        }
    }
    return any_unblocked;
}

struct DeadlockResult
{
    bool deadlock_detected = false;
    vector<int> deadlocked_process_indices;
};
//deadlock detection algorithm with banker's algorithm
DeadlockResult detect_deadlock_using_bankers(const vector<Process>& processes,
        const vector<int>& total_resources)
{
    int num_processes = (int)processes.size();
    int num_resources = (int)total_resources.size();

    vector<int> work = compute_available_resources(total_resources, processes);

    vector<int> can_finish(num_processes, 0);
    for (int i = 0; i < num_processes; i++)
    {
        if (processes[i].state == FINISHED || processes[i].state == TERMINATED)
            can_finish[i] = 1;
    }

    vector<vector<int>> needed_resources(num_processes, vector<int>(num_resources, 0));
    for (int i = 0; i < num_processes; i++)
    {
        if (can_finish[i]) continue;
        if (processes[i].state == BLOCKED_RESOURCE && processes[i].waiting_resource_index >= 0)
        {
            int resource_index = processes[i].waiting_resource_index;
            if (0 <= resource_index && resource_index < num_resources)
                needed_resources[i][resource_index] = processes[i].waiting_resource_count;
        }
    }

    bool progress = true;
    while (progress)
    {
        progress = false;
        for (int i = 0; i < num_processes; i++)
        {
            if (can_finish[i]) continue;

            bool can_complete = true;
            for (int k = 0; k < num_resources; k++)
            {
                if (needed_resources[i][k] > work[k])
                {
                    can_complete = false;
                    break;
                }
            }

            if (can_complete)
            {
                for (int k = 0; k < num_resources; k++)
                    work[k] += processes[i].allocated_resources[k];
                can_finish[i] = 1;
                progress = true;
            }
        }
    }

    DeadlockResult result;
    for (int i = 0; i < num_processes; i++)
    {
        if (!can_finish[i] && processes[i].state == BLOCKED_RESOURCE && !processes[i].is_done())
        {
            result.deadlocked_process_indices.push_back(i);
        }
    }

    result.deadlock_detected = !result.deadlocked_process_indices.empty();
    sort(result.deadlocked_process_indices.begin(), result.deadlocked_process_indices.end(),
         [&](int a, int b)
    {
        return processes[a].process_id < processes[b].process_id;
    });

    return result;
}
// choose victim for deadlock recovery
int choose_deadlock_victim(const vector<Process>& processes,
                           const vector<int>& deadlocked_indices)
{
    int victim_index = deadlocked_indices[0];
    for (int index : deadlocked_indices)
    {
        if (processes[index].base_priority > processes[victim_index].base_priority)
            victim_index = index;
        else if (processes[index].base_priority == processes[victim_index].base_priority &&
                 processes[index].process_id > processes[victim_index].process_id)
            victim_index = index;
    }
    return victim_index;
}

static void release_all_resources(Process &process)   //function to release resources when required
{
    for (int &allocated : process.allocated_resources) allocated = 0;
    process.waiting_resource_index = -1;
    process.waiting_resource_count = 0;
}

void recover_from_deadlock(vector<Process>& processes,
                           const vector<int>& total_resources,
                           const vector<Resource>& resources,
                           int current_time,
                           int &finished_count,
                           int &running_process_index)
{
    if (try_unblock_resource_waiters(processes, total_resources, resources, current_time))
    {
        rebuild_ready_queues(processes);
        return;
    }

    DeadlockResult detection = detect_deadlock_using_bankers(processes, total_resources);
    if (!detection.deadlock_detected) return;

    cout << "\n===== DEADLOCK (UNSAFE STATE) DETECTED at time " << current_time << " =====\n";
    cout << "Processes that cannot be completed safely now: ";
    for (int index : detection.deadlocked_process_indices)
    {
        Process &process = processes[index];
        int display_resource_id = (process.waiting_resource_index >= 0 &&
                                   process.waiting_resource_index < (int)resources.size())
                                  ? resources[process.waiting_resource_index].id : process.waiting_resource_index;
        cout << "P" << process.process_id << "(waits R" << display_resource_id
             << "[" << process.waiting_resource_count << "]) ";
    }
    cout << "\n";

    int victim_index = choose_deadlock_victim(processes, detection.deadlocked_process_indices);
    if (running_process_index == victim_index) running_process_index = -1;

    cout << "Deadlock recovery: Terminating P" << processes[victim_index].process_id
         << " (priority " << processes[victim_index].base_priority << ")\n";
    cout << "  Releasing resources: ";
    for (int k = 0; k < (int)processes[victim_index].allocated_resources.size(); k++)
    {
        if (processes[victim_index].allocated_resources[k] > 0)
        {
            int display_resource_id = (k < (int)resources.size()) ? resources[k].id : k;
            cout << "R" << display_resource_id << "["
                 << processes[victim_index].allocated_resources[k] << "] ";
        }
    }
    cout << "\n";

    release_all_resources(processes[victim_index]);
    processes[victim_index].state = TERMINATED;
    processes[victim_index].finish_time = current_time;
    finished_count++;

    try_unblock_resource_waiters(processes, total_resources, resources, current_time);
    rebuild_ready_queues(processes);

    cout << "===== END DEADLOCK RECOVERY =====\n\n";
}
// calculating ticks for io
void tick_io_for_process(vector<Process>& processes, int process_index,
                         int current_time, int &finished_count)
{
    Process &process = processes[process_index];
    if (process.state != BLOCKED_IO) return;

    // IO starts consuming only at io_start_time
    if (process.io_start_time != -1 && current_time < process.io_start_time) return;

    process.remaining_io_time--;

    if (process.remaining_io_time <= 0)
    {
        process.current_burst_index++;
        process.current_step_index = 0;
        process.remaining_cpu_time = 0;
        process.remaining_io_time = 0;
        process.io_start_time = -1;

        if (process.current_burst_index >= (int)process.bursts.size())
        {
            release_all_resources(process);
            process.state = FINISHED;
            process.finish_time = current_time ;
            finished_count++;
            return;
        }

        // IO return
        push_ready_reset_to_base_priority(processes, process_index, current_time );
    }
}

static void settle_after_boundary(vector<Process>& processes,Process &process,int boundary_time,
const vector<int>& total_resources,const unordered_map<int,int>& resource_id_to_index,int &finished_count)
{
    while (true)
    {
        if (process.is_done() || process.state == BLOCKED_IO ||
                process.state == BLOCKED_RESOURCE) return;

        if (process.current_burst_index >= (int)process.bursts.size())
        {
            release_all_resources(process);
            process.state = FINISHED;
            process.finish_time = boundary_time;
            finished_count++;
            return;
        }

        if (process.bursts[process.current_burst_index].type == "IO")
        {
            process.state = BLOCKED_IO;
            process.remaining_io_time = process.bursts[process.current_burst_index].io_duration;
            process.io_start_time = boundary_time;
            return;
        }

        auto &steps = process.bursts[process.current_burst_index].steps;
        if (process.current_step_index >= (int)steps.size())
        {
            process.current_burst_index++;
            process.current_step_index = 0;
            process.remaining_cpu_time = 0;
            continue;
        }

        InstructionStep step = steps[process.current_step_index];
        if (step.type == 'T') return;

        if (step.type == 'F')
        {
            int resource_index = resource_id_to_index.at(step.resource_id);
            process.allocated_resources[resource_index] -= step.resource_count;
            if (process.allocated_resources[resource_index] < 0)
                process.allocated_resources[resource_index] = 0;
            process.current_step_index++;
            continue;
        }

        if (step.type == 'R')
        {
            int resource_index = resource_id_to_index.at(step.resource_id);
            vector<int> available = compute_available_resources(total_resources, processes);
            if (available[resource_index] >= step.resource_count)
            {
                process.allocated_resources[resource_index] += step.resource_count;
                process.current_step_index++;
                continue;
            }
            else
            {
                set_blocked_on_resource(process, resource_index, step.resource_count);
                return;
            }
        }

        process.current_step_index++;
    }
}

bool execute_one_cpu_unit(vector<Process>& processes,
                          int process_index,
                          int current_time,
                          const vector<int>& total_resources,
                          const unordered_map<int,int>& resource_id_to_index,
                          int &finished_count)
{
    Process &process = processes[process_index];
    if (process.is_done()) return false;

    if (process.current_burst_index >= (int)process.bursts.size())
    {
        release_all_resources(process);
        process.state = FINISHED;
        process.finish_time = current_time;
        finished_count++;
        return false;
    }

    // If current burst is IO, go to IO starting next tick
    if (process.bursts[process.current_burst_index].type == "IO")
    {
        process.state = BLOCKED_IO;
        process.remaining_io_time = process.bursts[process.current_burst_index].io_duration;
        process.io_start_time = current_time + 1;
        return false;
    }

    auto &steps = process.bursts[process.current_burst_index].steps;

    while (true)
    {
        if (process.current_step_index >= (int)steps.size())
        {
            process.current_burst_index++;
            process.current_step_index = 0;
            process.remaining_cpu_time = 0;

            if (process.current_burst_index >= (int)process.bursts.size())
            {
                release_all_resources(process);
                process.state = FINISHED;
                process.finish_time = current_time;
                finished_count++;
                return false;
            }

            if (process.bursts[process.current_burst_index].type == "IO")
            {
                process.state = BLOCKED_IO;
                process.remaining_io_time = process.bursts[process.current_burst_index].io_duration;
                process.io_start_time = current_time + 1;
                return false;
            }
            continue;
        }

        InstructionStep step = steps[process.current_step_index];

        if (step.type == 'T')
        {
            if (process.remaining_cpu_time == 0)
                process.remaining_cpu_time = step.time_value;
            break;
        }

        if (step.type == 'F')
        {
            int resource_index = resource_id_to_index.at(step.resource_id);
            process.allocated_resources[resource_index] -= step.resource_count;
            if (process.allocated_resources[resource_index] < 0)
                process.allocated_resources[resource_index] = 0;
            process.current_step_index++;
            continue;
        }

        if (step.type == 'R')
        {
            int resource_index = resource_id_to_index.at(step.resource_id);
            vector<int> available = compute_available_resources(total_resources, processes);
            if (available[resource_index] >= step.resource_count)
            {
                process.allocated_resources[resource_index] += step.resource_count;
                process.current_step_index++;
                continue;
            }
            else
            {
                set_blocked_on_resource(process, resource_index, step.resource_count);
                return false;
            }
        }

        process.current_step_index++;
    }

    // Consume one CPU unit
    process.remaining_cpu_time--;
    process.round_robin_used++;

    if (process.remaining_cpu_time == 0)
    {
        process.current_step_index++;
        process.remaining_cpu_time = 0;
        settle_after_boundary(processes, process, current_time + 1,
                              total_resources, resource_id_to_index, finished_count);
    }

    return true;
}

struct GanttSegment
{
    int start_time;
    int end_time;
    int process_id;
};

void record_cpu_state_change(int current_process_id, int current_time,
                             int &last_process_id, int &last_change_time,
                             vector<GanttSegment> &gantt_chart)
{
    if (current_process_id != last_process_id)
    {
        if (last_process_id != -2)
            gantt_chart.push_back({last_change_time, current_time, last_process_id});
        last_process_id = current_process_id;
        last_change_time = current_time;
    }
}

void print_gantt_chart(const vector<GanttSegment> &gantt_chart)
{
    cout << "\n========== GANTT CHART ==========\n";
    for (auto &segment : gantt_chart)
    {
        cout << "[" << segment.start_time << "-" << segment.end_time << "] ";
        if (segment.process_id == -1) cout << "IDLE\n";
        else cout << "P" << segment.process_id << "\n";
    }
    cout << "\n";
}

void print_process_metrics(const vector<Process> &processes)
{
    cout << "\n========== PROCESS Details ==========\n";
    cout << "PID\tArrival\tFinish\tWaiting\tTurnaround\tState\n";

    long long total_waiting = 0;
    long long total_turnaround = 0;
    int completed_count = 0;

    for (auto &process : processes)
    {
        long long turnaround_time = (process.finish_time >= 0)
                                    ? (process.finish_time - process.arrival_time) : 0;
        cout << process.process_id << "\t" << process.arrival_time << "\t"
             << process.finish_time << "\t"
             << process.total_waiting_time << "\t" << turnaround_time << "\t\t"
             << get_state_name(process.state) << "\n";

        if (process.state == FINISHED || process.state == TERMINATED)
        {
            total_waiting += process.total_waiting_time;
            total_turnaround += turnaround_time;
            completed_count++;
        }
    }
if (running_process_index == -1 && !has_any_ready_process(
    if (completed_count > 0)
    {
        cout << "\nAverage Waiting Time: "
             << (double)total_waiting / completed_count << "\n";
        cout << "Average Turnaround Time: "
             << (double)total_turnaround / completed_count << "\n";
    }
    cout << "\n";
}

int main()
{
    string input_filename = "input.txt";
    ifstream input_file(input_filename);
    if (!input_file.is_open())
    {
        cout << "Error: Cannot open file: " << input_filename << "\n";
        return 1;
    }

    string line;
    if (!getline(input_file, line))
    {
        cout << "Error: Empty file!\n";
        return 1;
    }

    vector<Resource> resources = parse_resources_line(line);
    unordered_map<int,int> resource_id_to_index;
    vector<int> total_resources;
    for (int i = 0; i < (int)resources.size(); i++)
    {
        resource_id_to_index[resources[i].id] = i;
        total_resources.push_back(resources[i].instances);
    }

    cout << "========== CPU scheduling system ==========\n";
    cout << "Resources:\n";
    for (auto &resource : resources)
        cout << "  R" << resource.id << " = " << resource.instances << " instances\n";

    vector<Process> processes;
    while (getline(input_file, line))
    {
        if (line.empty()) continue;
        Process process;
        if (parse_process_line(line, process))
        {
            process.bursts = parse_bursts(process.bursts_text);
            process.allocated_resources.assign((int)total_resources.size(), 0);
            process.state = NEW;
            process.finish_time = -1;
            processes.push_back(process);
        }
    }
    input_file.close();

    sort(processes.begin(), processes.end(), [](const Process &a, const Process &b)
    {
        if (a.arrival_time != b.arrival_time) return a.arrival_time < b.arrival_time;
        if (a.base_priority != b.base_priority) return a.base_priority < b.base_priority;
        return a.process_id < b.process_id;
    });

    cout << "\nProcesses number = " << processes.size() << "\n";
    for (auto &process : processes)
    {
        cout << "PID=" << process.process_id << " Arrival=" << process.arrival_time
             << " Priority=" << process.base_priority
             << " Bursts=[" << process.bursts_text << "]\n";
        cout << "\n";
    }

    int current_time = 0;
    int running_process_index = -1;
    int finished_count = 0;
    int next_arrival_index = 0;

    vector<GanttSegment> gantt_chart;
    int last_cpu_process_id = -2;
    int last_state_change_time = 0;

    const int MAX_SIMULATION_TIME = 200000;

    while (finished_count < (int)processes.size() && current_time < MAX_SIMULATION_TIME)
    {

        while (next_arrival_index < (int)processes.size() &&
                processes[next_arrival_index].arrival_time == current_time)
        {
            push_ready_reset_to_base_priority(processes, next_arrival_index, current_time);
            next_arrival_index++;
        }

        // Tick IO for all blocked processes
        for (int i = 0; i < (int)processes.size(); i++)
            if (processes[i].state == BLOCKED_IO)
                tick_io_for_process(processes, i, current_time, finished_count);

        // AGING: do NOT age on the same tick the process entered READY
        bool priority_changed = false;
        for (auto &process : processes)
        {
            if (process.state == READY)
            {
                if (process.ready_enter_time != -1 && current_time > process.ready_enter_time)
                {
                    process.ready_wait_ticks++;
                    if (process.ready_wait_ticks >= AGING_TICK_LIMIT)
                    {
                        int old_priority = process.current_priority;
                        process.current_priority = max(PRIORITY_MIN, process.current_priority - 1);
                        process.ready_wait_ticks = 0;
                        if (process.current_priority != old_priority) priority_changed = true;
                    }
                }
            }
        }
        if (priority_changed) rebuild_ready_queues(processes);

        // Try unblock resources
        if (try_unblock_resource_waiters(processes, total_resources, resources, current_time))
        {
            rebuild_ready_queues(processes);
        }

        // PREEMPT if someone now has better priority
        int best_priority = find_best_ready_priority();
        if (running_process_index != -1 && best_priority != -1)
        {
            if (best_priority < processes[running_process_index].current_priority)
            {
                push_ready_keep_aged_priority(processes, running_process_index, current_time);
                running_process_index = -1;
            }
        }

        if (running_process_index != -1)
        {
            int running_priority = processes[running_process_index].current_priority;
            if (!ready_queues[running_priority].empty() &&
                    processes[running_process_index].round_robin_used >= TIME_QUANTUM)
            {
                push_ready_keep_aged_priority(processes, running_process_index, current_time);
                running_process_index = -1;
            }
        }

        if (running_process_index == -1)
        {
            running_process_index = pop_ready_process(processes, current_time);
        }

        // Deadlock check if idle and nothing ready
        if (running_process_index == -1 && !has_any_ready_process())
        {
            if (!has_any_io_waiting_process(processes) &&
                    all_done_or_blocked_on_resource(processes))
            {
                recover_from_deadlock(processes, total_resources, resources,
                                      current_time, finished_count, running_process_index);
                running_process_index = pop_ready_process(processes, current_time);
            }
        }

        int running_pid_this_tick = -1;

        // Execute one CPU unit
        if (running_process_index != -1)
        {
            bool executed = execute_one_cpu_unit(processes, running_process_index,
                                                 current_time, total_resources,
                                                 resource_id_to_index, finished_count);
            if (executed) running_pid_this_tick = processes[running_process_index].process_id;

            if (processes[running_process_index].state == BLOCKED_IO ||
                    processes[running_process_index].state == BLOCKED_RESOURCE ||
                    processes[running_process_index].state == FINISHED ||
                    processes[running_process_index].state == TERMINATED)
            {
                running_process_index = -1;
            }
        }
        record_cpu_state_change(running_pid_this_tick == -1 ? -1 : running_pid_this_tick,current_time, last_cpu_process_id,
                                last_state_change_time, gantt_chart);

        current_time++;
    }

    record_cpu_state_change(-9999, current_time, last_cpu_process_id,
                            last_state_change_time, gantt_chart);
    if (!gantt_chart.empty() && gantt_chart.back().process_id == -9999)
        gantt_chart.pop_back();

    print_process_metrics(processes);
    print_gantt_chart(gantt_chart);
    return 0;
}
