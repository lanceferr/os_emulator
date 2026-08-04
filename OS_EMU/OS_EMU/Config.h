#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

enum class SchedulerType { FCFS, RR };
enum class MemScheme { FLAT, PAGING };

struct Config {
    int numCPU = 4;
    SchedulerType scheduler = SchedulerType::FCFS;
    uint32_t quantumCycles = 1;
    uint32_t batchProcessFreq = 1;
    uint32_t minIns = 1;
    uint32_t maxIns = 1;
    uint32_t delaysPerExec = 0;

    // Memory manager (Week 10 + MO2)
    uint32_t maxOverallMem = 16384;
    uint32_t memPerFrame = 16;
    // MO2: memory given to scheduler-generated processes is rolled between
    // these two (both must be a power of two in [2^6, 2^16]).
    uint32_t minMemPerProc = 4096;
    uint32_t maxMemPerProc = 4096;
    MemScheme memScheme = MemScheme::FLAT; // "flat" (first-fit) or "paging"

    bool loaded = false;
};

// MO2: all process/frame memory sizes must be a power of two in [2^6, 2^16] bytes.
inline bool isValidMemSize(long long v) {
    if (v < 2 || v > 65536) return false;
    return (v & (v - 1)) == 0;
}

class ConfigLoader {
public:
    // Returns true on success. On failure, prints an error and leaves config unloaded.
    static bool load(const std::string& path, Config& out) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cout << "Error: could not open '" << path << "'.\n";
            return false;
        }

        std::string key;
        while (file >> key) {
            if (key == "num-cpu") {
                int v; file >> v;
                if (v < 1 || v > 128) {
                    std::cout << "Error: num-cpu must be in [1, 128].\n";
                    return false;
                }
                out.numCPU = v;
            }
            else if (key == "scheduler") {
                std::string v; file >> v;
                // values may come quoted, e.g. "fcfs"
                if (!v.empty() && v.front() == '"') v.erase(0, 1);
                if (!v.empty() && v.back() == '"') v.pop_back();
                if (v == "fcfs") out.scheduler = SchedulerType::FCFS;
                else if (v == "rr") out.scheduler = SchedulerType::RR;
                else {
                    std::cout << "Error: scheduler must be 'fcfs' or 'rr'.\n";
                    return false;
                }
            }
            else if (key == "quantum-cycles") {
                long v; file >> v;
                out.quantumCycles = static_cast<uint32_t>(v);
            }
            else if (key == "batch-process-freq") {
                long v; file >> v;
                out.batchProcessFreq = static_cast<uint32_t>(v);
            }
            else if (key == "min-ins") {
                long v; file >> v;
                out.minIns = static_cast<uint32_t>(v);
            }
            else if (key == "max-ins") {
                long v; file >> v;
                out.maxIns = static_cast<uint32_t>(v);
            }
            else if (key == "delays-per-exec") {
                long v; file >> v;
                out.delaysPerExec = static_cast<uint32_t>(v);
            }
            else if (key == "max-overall-mem") {
                long v; file >> v;
                out.maxOverallMem = static_cast<uint32_t>(v);
            }
            else if (key == "mem-per-frame") {
                long v; file >> v;
                out.memPerFrame = static_cast<uint32_t>(v);
            }
            else if (key == "mem-per-proc") {
                // Backward-compat single value: treat as min == max.
                long v; file >> v;
                if (!isValidMemSize(v)) {
                    std::cout << "Error: mem-per-proc must be a power of two in [64, 65536].\n";
                    return false;
                }
                out.minMemPerProc = static_cast<uint32_t>(v);
                out.maxMemPerProc = static_cast<uint32_t>(v);
            }
            else if (key == "min-mem-per-proc") {
                long v; file >> v;
                if (!isValidMemSize(v)) {
                    std::cout << "Error: min-mem-per-proc must be a power of two in [64, 65536].\n";
                    return false;
                }
                out.minMemPerProc = static_cast<uint32_t>(v);
            }
            else if (key == "max-mem-per-proc") {
                long v; file >> v;
                if (!isValidMemSize(v)) {
                    std::cout << "Error: max-mem-per-proc must be a power of two in [64, 65536].\n";
                    return false;
                }
                out.maxMemPerProc = static_cast<uint32_t>(v);
            }
            else if (key == "mem-scheme") {
                std::string v; file >> v;
                if (!v.empty() && v.front() == '"') v.erase(0, 1);
                if (!v.empty() && v.back() == '"') v.pop_back();
                if (v == "paging") out.memScheme = MemScheme::PAGING;
                else if (v == "flat") out.memScheme = MemScheme::FLAT;
                else {
                    std::cout << "Error: mem-scheme must be 'flat' or 'paging'.\n";
                    return false;
                }
            }
            else {
                // Unknown key: skip its value token and continue (forward-compatible)
                std::string skip; file >> skip;
            }
        }

        if (out.minIns > out.maxIns) {
            std::cout << "Error: min-ins cannot exceed max-ins.\n";
            return false;
        }
        if (out.minMemPerProc > out.maxMemPerProc) {
            std::cout << "Error: min-mem-per-proc cannot exceed max-mem-per-proc.\n";
            return false;
        }

        out.loaded = true;
        return true;
    }
};