#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdint>
#include "Instruction.h"

enum class ProcessState {
    READY,
    RUNNING,
    WAITING,   // sleeping due to SLEEP instruction
    FINISHED
};

// A flattened "program counter" position is awkward with nested FOR loops,
// so instead we track an execution cursor as a stack of (instruction list, index, remaining repeats).
// This lets us pause mid-FOR-loop when a quantum expires (Round Robin) and resume later.
struct ExecFrame {
    const std::vector<Instruction>* list;
    size_t index;
    int repeatsLeft; // only meaningful for FOR frames; ignored for the root frame
};

class Process {
public:
    std::string name;
    int pid;
    ProcessState state;
    int coreId;
    std::string createdAt;
    std::string finishedAt;
    std::mutex mtx;

    // Full program for this process (top-level instruction list)
    std::vector<Instruction> program;

    // Execution position: stack-based so FOR loops can be paused/resumed
    std::vector<ExecFrame> execStack;

    // Total instructions executed so far (for progress display, e.g. "5 / 100")
    int instructionsExecuted;
    int totalInstructions; // total flat count, including FOR loop bodies expanded

    // In-memory variable store, per spec: uint16 clamped to [0, 65535]
    std::unordered_map<std::string, uint16_t> variables;

    // In-memory log instead of writing to a .txt file per process
    std::vector<std::string> logs;

    // SLEEP bookkeeping: ticks remaining before this process can run again
    int sleepTicksRemaining;

    Process(const std::string& name, int pid, std::vector<Instruction> programIn, int totalFlatCount)
        : name(name), pid(pid), state(ProcessState::READY), coreId(-1),
        instructionsExecuted(0), totalInstructions(totalFlatCount),
        sleepTicksRemaining(0) {
        program = std::move(programIn);
        createdAt = getCurrentTimestamp();
        execStack.push_back({ &program, 0, 0 });
    }

    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_info;
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        std::tm* tmp = std::localtime(&t);
        if (tmp) tm_info = *tmp;
#endif
        std::ostringstream oss;
        oss << "("
            << std::setfill('0') << std::setw(2) << (tm_info.tm_mon + 1) << "/"
            << std::setw(2) << tm_info.tm_mday << "/"
            << (tm_info.tm_year + 1900) << " "
            << std::setw(2) << tm_info.tm_hour << ":"
            << std::setw(2) << tm_info.tm_min << ":"
            << std::setw(2) << tm_info.tm_sec;
        if (tm_info.tm_hour < 12) oss << "AM)";
        else oss << "PM)";
        return oss.str();
    }

    bool isFinished() const {
        return execStack.empty();
    }

    // Resolves an operand to its value, declaring the variable with 0 first if needed
    uint16_t resolve(const Operand& op) {
        if (op.isLiteral) return op.literalValue;
        auto it = variables.find(op.varName);
        if (it == variables.end()) {
            variables[op.varName] = 0; // auto-declare with 0 per spec
            return 0;
        }
        return it->second;
    }

    static uint16_t clampU16(int32_t value) {
        if (value < 0) return 0;
        if (value > 65535) return 65535;
        return static_cast<uint16_t>(value);
    }

    void appendLog(const std::string& text, int core) {
        std::lock_guard<std::mutex> lock(mtx);
        std::ostringstream oss;
        oss << getCurrentTimestamp() << " Core:" << core << " " << text;
        logs.push_back(oss.str());
    }

    // Executes exactly ONE flat instruction (resuming where execStack left off).
    // Returns true if an instruction was executed, false if the process is already finished.
    // If the instruction is SLEEP, sets sleepTicksRemaining and returns true (caller checks state).
    bool executeOneStep(int core) {
        while (!execStack.empty()) {
            ExecFrame& frame = execStack.back();

            if (frame.index >= frame.list->size()) {
                // This frame is done.
                if (execStack.size() == 1) {
                    // Root frame finished -> process complete
                    execStack.pop_back();
                    return false;
                }
                // It's a FOR frame: check repeats
                if (frame.repeatsLeft > 1) {
                    frame.repeatsLeft--;
                    frame.index = 0;
                    continue; // loop again from the top of the FOR body
                }
                else {
                    execStack.pop_back();
                    continue; // pop back to parent frame
                }
            }

            const Instruction& ins = (*frame.list)[frame.index];

            if (ins.type == InstructionType::FOR) {
                // Push a new frame for the loop body; don't advance parent index yet
                // until the loop fully completes (handled when child frame pops).
                frame.index++; // advance parent past this FOR for when we return
                execStack.push_back({ &ins.forBody, 0, ins.forRepeats });
                continue;
            }

            // Regular instruction: execute it, advance, and return.
            frame.index++;
            runInstruction(ins, core);
            instructionsExecuted++;
            return true;
        }
        return false;
    }

private:
    void runInstruction(const Instruction& ins, int core) {
        switch (ins.type) {
        case InstructionType::PRINT: {
            std::string msg = ins.printLiteralMsg;
            if (!ins.printVarName.empty()) {
                uint16_t v = resolve(Operand::fromVar(ins.printVarName));
                msg += " " + std::to_string(v);
            }
            appendLog("\"" + msg + "\"", core);
            break;
        }
        case InstructionType::DECLARE: {
            std::lock_guard<std::mutex> lock(mtx);
            variables[ins.declareVar] = ins.declareValue;
            break;
        }
        case InstructionType::ADD: {
            uint16_t a = resolve(ins.arithSrc1);
            uint16_t b = resolve(ins.arithSrc2);
            std::lock_guard<std::mutex> lock(mtx);
            variables[ins.arithDest] = clampU16((int32_t)a + (int32_t)b);
            break;
        }
        case InstructionType::SUBTRACT: {
            uint16_t a = resolve(ins.arithSrc1);
            uint16_t b = resolve(ins.arithSrc2);
            std::lock_guard<std::mutex> lock(mtx);
            variables[ins.arithDest] = clampU16((int32_t)a - (int32_t)b);
            break;
        }
        case InstructionType::SLEEP: {
            sleepTicksRemaining = ins.sleepTicks;
            state = ProcessState::WAITING;
            break;
        }
        default:
            break; // FOR is handled in executeOneStep, never reaches here
        }
    }
};