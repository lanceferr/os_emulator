// libraries
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <thread>
#include <fstream>
#include <random>

// header files
#include "frontend.h"
#include "Scheduler.h"
#include "Config.h"
#include "InstructionGenerator.h"
#include "IMemoryAllocator.h"
#include "MemoryAllocator.h"
#include "PagingAllocator.h"
#include "InstructionParser.h"
// Include concrete allocator headers so dynamic_cast<...> works at this translation unit.

static int pidCounter = 1;
static Config config;
static std::unique_ptr<Scheduler> scheduler;
static std::unique_ptr<IMemoryAllocator> memAlloc;
static InstructionGenerator insGen;

// Switchable instruction generation mode, controlled by the "gen-mode" command.
enum class GenMode { RANDOM, ALTERNATING_XY };
static GenMode genMode = GenMode::RANDOM;

static std::atomic<bool> batchGenActive(false);
static std::thread batchGenThread;
static std::atomic<int> batchPidSeq(1);

// Rolls a uniformly-random power-of-two byte size within [minSize, maxSize].
// Both bounds are guaranteed by ConfigLoader to already be powers of two in
// [64, 65536], so this just picks among the powers of two between them.
static uint32_t rollPow2MemSize(uint32_t minSize, uint32_t maxSize, std::mt19937& rng) {
    std::vector<uint32_t> options;
    for (uint32_t v = minSize; v <= maxSize; v <<= 1) options.push_back(v);
    if (options.empty()) return minSize;
    return options[rng() % options.size()];
}

// Counts every instruction in a program, including FOR loop bodies expanded
// by their repeat count. Used for progress display ("5 / N").
static int countFlatInstructions(const std::vector<Instruction>& list) {
    int total = 0;
    for (auto& ins : list) {
        if (ins.type == InstructionType::FOR) {
            total += ins.forRepeats * countFlatInstructions(ins.forBody);
        }
        else {
            total += 1;
        }
    }
    return total;
}

std::shared_ptr<Process> createProcess(const std::string& name, int numInstructions, size_t memSize) {
    std::vector<Instruction> program;
    // Default: randomized mix of DECLARE/ADD/SUBTRACT/SLEEP/PRINT/FOR/READ/WRITE.
    // Passing memSize lets the generator include READ/WRITE targeting
    // addresses within this process's own allocation, which is what
    // actually drives demand-paging activity (page faults) for
    // scheduler-generated processes.
    program = insGen.generate(name, numInstructions, memSize);

    int flatTotal = countFlatInstructions(program);
    auto p = std::make_shared<Process>(name, pidCounter++, std::move(program), flatTotal, memSize);
    return p;
}

// Used by "screen -c": builds a process from a user-supplied instruction
// string instead of the random generator.
std::shared_ptr<Process> createProcessFromInstructions(const std::string& name,
    std::vector<Instruction> program, size_t memSize) {
    int flatTotal = countFlatInstructions(program);
    auto p = std::make_shared<Process>(name, pidCounter++, std::move(program), flatTotal, memSize);
    return p;
}

void printHelp() {
    std::cout << "Available commands:\n";
    std::cout << "  initialize              - Load config.txt and start the scheduler\n";
    std::cout << "  screen -ls              - List all running and finished processes\n";
    std::cout << "  screen -s <name> <size> - Create a new process with the given memory size\n";
    std::cout << "  screen -c <name> <size> \"<instrs>\" - Create a process with explicit instructions\n";
    std::cout << "  screen -r <name>        - Reattach to an existing process\n";
    std::cout << "  process-smi             - Summarized memory/CPU usage overview\n";
    std::cout << "  vmstat                  - Detailed memory and CPU tick statistics\n";
    std::cout << "  scheduler-start         - Begin generating dummy processes\n";
    std::cout << "  scheduler-stop          - Stop generating dummy processes\n";
    std::cout << "  report-util             - Show CPU utilization, save to csopesy-log.txt\n";
    std::cout << "  gen-mode <random|altxy> - Switch instruction generation mode (current default: random)\n";
    std::cout << "  clear                   - Clear the screen\n";
    std::cout << "  exit                    - Exit the emulator\n\n";
}

static std::string fmtTime(const std::string& ts) {
    return ts.empty() ? "(unknown)" : ts;
}

// ---- screen -ls / report-util shared formatting ----

static std::string buildUtilReport() {
    std::ostringstream oss;
    auto running = scheduler->getRunningProcesses();
    auto finished = scheduler->getFinishedProcesses();
    int numCores = scheduler->getNumCores();
    int busy = scheduler->coresInUse();
    double util = (numCores > 0) ? (double)busy / numCores * 100.0 : 0.0;

    oss << "CPU utilization: " << std::fixed << std::setprecision(1) << util << "%\n";
    oss << "Cores used: " << busy << "\n";
    oss << "Cores available: " << (numCores - busy) << "\n";
    oss << "----------------------------------------------\n";
    oss << "Running processes:\n";

    bool anyRunning = false;
    for (int c = 0; c < numCores; c++) {
        if (running[c] != nullptr) {
            auto& p = running[c];
            std::lock_guard<std::mutex> lock(p->mtx);
            oss << std::left << std::setw(12) << p->name
                << "  " << fmtTime(p->createdAt)
                << "    Core: " << c
                << "    " << p->instructionsExecuted
                << " / " << p->totalInstructions << "\n";
            anyRunning = true;
        }
    }
    if (!anyRunning) oss << "  (none)\n";

    oss << "\nFinished processes:\n";
    if (finished.empty()) {
        oss << "  (none)\n";
    }
    else {
        for (auto& p : finished) {
            std::lock_guard<std::mutex> lock(p->mtx);
            oss << std::left << std::setw(12) << p->name
                << "  " << fmtTime(p->createdAt)
                << (p->memoryViolation ? "    Terminated" : "    Finished")
                << "    " << p->instructionsExecuted
                << " / " << p->totalInstructions << "\n";
        }
    }
    oss << "----------------------------------------------\n";

    // Memory manager summary, if attached. Branches on the concrete scheme
    // only to decide which extra stats to print; both schemes are reached
    // through the same IMemoryAllocator interface for everything else.
    if (memAlloc) {
        if (auto* flat = dynamic_cast<MemoryAllocator*>(memAlloc.get())) {
            oss << "Memory: " << flat->processesInMemory() << " process(es) allocated, "
                << flat->externalFragmentation() << " bytes external fragmentation\n";
        }
        else if (auto* paging = dynamic_cast<PagingAllocator*>(memAlloc.get())) {
            oss << "Memory: " << paging->processesInMemory() << " process(es) allocated, "
                << paging->externalFragmentation() << " bytes external fragmentation\n";
            oss << "Paging: " << paging->getPagesPagedIn() << " pages paged in, "
                << paging->getPagesPagedOut() << " pages paged out, "
                << paging->internalFragmentation() << " bytes internal fragmentation\n";
        }
        else {
            oss << memAlloc->visualizeMemory() << "\n";
        }
        oss << "----------------------------------------------\n";
    }

    return oss.str();
}

void printScreenLs() {
    std::cout << buildUtilReport();
}

// ---- main-menu "process-smi": nvidia-smi-style memory + process overview ----
static std::string buildProcessSmiOverview() {
    std::ostringstream oss;
    size_t totalMem = memAlloc ? memAlloc->getTotalMemory() : 0;
    size_t usedMem = memAlloc ? memAlloc->getUsedMemory() : 0;
    size_t freeMem = (totalMem >= usedMem) ? (totalMem - usedMem) : 0;
    double memUtil = totalMem > 0 ? (double)usedMem / (double)totalMem * 100.0 : 0.0;

    int numCores = scheduler->getNumCores();
    int busy = scheduler->coresInUse();
    double cpuUtil = numCores > 0 ? (double)busy / numCores * 100.0 : 0.0;

    oss << "==============================================\n";
    oss << "| PROCESS-SMI V01.00   Driver Version: 01.00 |\n";
    oss << "==============================================\n";
    oss << "CPU-Util: " << std::fixed << std::setprecision(0) << cpuUtil << "%\n";
    oss << "Memory Usage: " << usedMem << "B / " << totalMem << "B\n";
    oss << "Memory Util: " << std::fixed << std::setprecision(0) << memUtil << "%\n";
    oss << "Free Memory: " << freeMem << "B\n";
    oss << "----------------------------------------------\n";
    oss << "Running processes and memory usage:\n";
    oss << "----------------------------------------------\n";

    auto running = scheduler->getRunningProcesses();
    bool any = false;
    for (auto& p : running) {
        if (!p) continue;
        std::lock_guard<std::mutex> lock(p->mtx);
        oss << std::left << std::setw(12) << p->name
            << std::right << std::setw(8) << p->requestedMemSize << "B\n";
        any = true;
    }
    if (!any) oss << "  (none)\n";
    oss << "----------------------------------------------\n";
    return oss.str();
}

// ---- main-menu "vmstat": detailed memory + CPU tick statistics ----
static std::string buildVmstat() {
    std::ostringstream oss;
    size_t totalMem = memAlloc ? memAlloc->getTotalMemory() : 0;
    size_t usedMem = memAlloc ? memAlloc->getUsedMemory() : 0;
    size_t freeMem = (totalMem >= usedMem) ? (totalMem - usedMem) : 0;

    uint64_t idleTicks = scheduler->getIdleCpuTicks();
    uint64_t activeTicks = scheduler->getActiveCpuTicks();
    uint64_t totalTicks = scheduler->getTotalCpuTicks();

    int pagedIn = 0, pagedOut = 0;
    if (memAlloc) {
        if (auto* paging = dynamic_cast<PagingAllocator*>(memAlloc.get())) {
            pagedIn = paging->getPagesPagedIn();
            pagedOut = paging->getPagesPagedOut();
        }
    }

    oss << std::right << std::setw(12) << totalMem << "  total memory (bytes)\n";
    oss << std::right << std::setw(12) << usedMem << "  used memory (bytes)\n";
    oss << std::right << std::setw(12) << freeMem << "  free memory (bytes)\n";
    oss << std::right << std::setw(12) << idleTicks << "  idle cpu ticks\n";
    oss << std::right << std::setw(12) << activeTicks << "  active cpu ticks\n";
    oss << std::right << std::setw(12) << totalTicks << "  total cpu ticks\n";
    oss << std::right << std::setw(12) << pagedIn << "  num paged in\n";
    oss << std::right << std::setw(12) << pagedOut << "  num paged out\n";
    return oss.str();
}

void reportUtil() {
    std::string report = buildUtilReport();
    std::cout << report;

    std::ofstream out("csopesy-log.txt");
    if (out.is_open()) {
        out << report;
        out.close();
        std::cout << "Report saved to csopesy-log.txt\n";
    }
    else {
        std::cout << "Warning: could not write csopesy-log.txt\n";
    }
}

// ---- screen -r <name>: attach to a process and show process-smi / exit ----

static std::shared_ptr<Process> findProcess(const std::string& name,
    const std::vector<std::shared_ptr<Process>>& all) {
    for (auto& p : all) {
        if (p->name == name) return p;
    }
    return nullptr;
}

static void processSmi(std::shared_ptr<Process> p) {
    std::lock_guard<std::mutex> lock(p->mtx);
    std::cout << "Process name: " << p->name << "\n";
    std::cout << "ID: " << p->pid << "\n";
    std::cout << "Logs:\n";
    for (auto& line : p->logs) {
        std::cout << line << "\n";
    }
    std::cout << "\nCurrent instruction line: " << p->instructionsExecuted << "\n";
    std::cout << "Lines of code: " << p->totalInstructions << "\n";
    if (p->memoryViolation) {
        std::ostringstream oss;
        oss << std::hex << std::showbase << p->violationAddress;
        std::cout << "Process " << p->name << " shut down due to memory access violation error that occurred at "
            << p->violationTimestamp << ". " << oss.str() << " invalid.\n";
    }
    else if (p->state == ProcessState::FINISHED) {
        std::cout << "Finished!\n";
    }
}

static void enterScreen(std::shared_ptr<Process> p) {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "Attached to process '" << p->name << "'. Type 'process-smi' or 'exit'.\n";
    std::string line;
    while (true) {
        std::cout << "[" << p->name << "] Enter a command: ";
        if (!std::getline(std::cin, line)) break;
        if (line == "exit") {
#ifdef _WIN32
            system("cls");
#else
            system("clear");
#endif
            printMenu();
            break;
        }
        else if (line == "process-smi") {
            if (p->state == ProcessState::FINISHED) {
                processSmi(p);
                std::cout << (p->memoryViolation
                    ? "(Process terminated. Returning to main menu.)\n"
                    : "(Process finished. Returning to main menu.)\n");
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                printMenu();
                break;
            }
            processSmi(p);
        }
        else {
            std::cout << "Unknown command inside screen. Try 'process-smi' or 'exit'.\n";
        }
    }
}

// ---- scheduler-start: generates a new dummy process every batch-process-freq ticks ----

static void batchGenLoop(std::vector<std::shared_ptr<Process>>* allProcesses, std::mutex* allMutex) {
    std::mt19937 rng(std::random_device{}());
    while (batchGenActive.load()) {
        int numInstr = config.minIns;
        if (config.maxIns > config.minIns) {
            numInstr = config.minIns + (rng() % (config.maxIns - config.minIns + 1));
        }
        std::string pname = "p" + std::to_string(batchPidSeq.load());
        // zero-pad to at least 2 digits, per the p01, p02... example in the spec
        {
            std::ostringstream oss;
            oss << "p" << std::setw(2) << std::setfill('0') << batchPidSeq.load();
            pname = oss.str();
        }
        batchPidSeq++;

        uint32_t memSize = rollPow2MemSize(config.minMemPerProc, config.maxMemPerProc, rng);
        auto p = createProcess(pname, numInstr, memSize);
        {
            std::lock_guard<std::mutex> lock(*allMutex);
            allProcesses->push_back(p);
        }
        scheduler->addProcess(p);

        // Sleep approximates "every X CPU ticks" using a fixed wall-clock interval per tick.
        for (uint32_t i = 0; i < config.batchProcessFreq && batchGenActive.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

int main() {
    printMenu();

    bool initialized = false;
    std::vector<std::shared_ptr<Process>> allProcesses;
    std::mutex allProcessesMutex;
    std::string line;

    while (true) {
        std::cout << "Enter a command: ";
        if (!std::getline(std::cin, line)) break;

        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
            line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line.empty()) continue;

        // "exit" is always allowed, even before initialize.
        if (line == "exit") {
            if (batchGenActive.load()) {
                batchGenActive.store(false);
                if (batchGenThread.joinable()) batchGenThread.join();
            }
            if (scheduler) scheduler->stop();
            std::cout << "Goodbye!\n";
            break;
        }

        // Per spec: nothing except "exit" works until "initialize" has run.
        if (!initialized) {
            if (line == "initialize") {
                if (!ConfigLoader::load("config.txt", config)) {
                    std::cout << "Initialization failed. Fix config.txt and try again.\n";
                    continue;
                }
                scheduler = std::make_unique<Scheduler>(
                    config.numCPU, config.scheduler, config.quantumCycles, config.delaysPerExec);

                // Attach a memory allocator if memory config is present. The
                // concrete scheme (flat/first-fit vs. paging) is chosen by
                // config.memScheme; Scheduler only ever sees IMemoryAllocator.
                if (config.maxOverallMem > 0 && config.maxMemPerProc > 0) {
                    if (config.memScheme == MemScheme::PAGING) {
                        memAlloc = std::make_unique<PagingAllocator>(
                            config.maxOverallMem, config.maxMemPerProc, config.memPerFrame);
                        std::cout << "Memory allocator: paging (demand), " << config.maxOverallMem
                            << " bytes total, " << config.memPerFrame << " bytes per frame.\n";
                    }
                    else {
                        memAlloc = std::make_unique<MemoryAllocator>(
                            config.maxOverallMem, config.maxMemPerProc, config.memPerFrame);
                        std::cout << "Memory allocator: flat/first-fit, " << config.maxOverallMem
                            << " bytes total.\n";
                    }
                    // minMemPerProc/maxMemPerProc default is only used as a fallback
                    // for processes with no per-process size set; each process
                    // normally carries its own requestedMemSize (see screen -s/-c
                    // and batchGenLoop).
                    scheduler->setMemoryAllocator(memAlloc.get(), config.minMemPerProc);
                }

                scheduler->start();
                initialized = true;
                std::cout << "Initialized with " << config.numCPU << " core(s), scheduler="
                    << (config.scheduler == SchedulerType::FCFS ? "fcfs" : "rr") << ".\n";
            }
            else if (line == "help") {
                printHelp();
            }
            else {
                std::cout << "Please run 'initialize' first.\n";
            }
            continue;
        }

        // ---- commands available after initialize ----

        if (line == "initialize") {
            std::cout << "Already initialized.\n";
        }
        else if (line == "help") {
            printHelp();
        }
        else if (line == "clear") {
#ifdef _WIN32
            system("cls");
#else
            system("clear");
#endif
            printMenu();
        }
        else if (line == "screen -ls") {
            printScreenLs();
        }
        else if (line == "process-smi") {
            std::cout << buildProcessSmiOverview();
        }
        else if (line == "vmstat") {
            if (auto* paging = dynamic_cast<PagingAllocator*>(memAlloc.get())) {
                paging->flushBackingStoreFile();
            }
            std::cout << buildVmstat();
        }
        else if (line.rfind("screen -s ", 0) == 0) {
            std::string rest = line.substr(10);
            std::istringstream iss(rest);
            std::string pname, sizeTok;
            iss >> pname >> sizeTok;
            if (pname.empty() || sizeTok.empty()) {
                std::cout << "Usage: screen -s <process_name> <process_memory_size>\n";
            }
            else {
                long long size = 0;
                bool parsedOk = true;
                try { size = std::stoll(sizeTok); }
                catch (...) { parsedOk = false; }

                if (!parsedOk || !isValidMemSize(size)) {
                    std::cout << "Invalid memory allocation.\n";
                }
                else {
                    std::lock_guard<std::mutex> lock(allProcessesMutex);
                    if (findProcess(pname, allProcesses) != nullptr) {
                        std::cout << "Process '" << pname << "' already exists.\n";
                    }
                    else {
                        int numInstr = config.minIns;
                        if (config.maxIns > config.minIns) {
                            numInstr = config.minIns + (rand() % (config.maxIns - config.minIns + 1));
                        }
                        auto p = createProcess(pname, numInstr, static_cast<size_t>(size));
                        allProcesses.push_back(p);
                        scheduler->addProcess(p);
                        std::cout << "Process '" << pname << "' created and added to queue.\n";
                    }
                }
            }
        }
        else if (line.rfind("screen -c ", 0) == 0) {
            std::string rest = line.substr(10);

            // Split on the first/last double-quote: everything before is
            // "<name> <size>", everything between is the instruction text.
            size_t q1 = rest.find('"');
            size_t q2 = (q1 == std::string::npos) ? std::string::npos : rest.rfind('"');

            std::string pname, sizeTok, instrText;
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                std::istringstream hiss(rest.substr(0, q1));
                hiss >> pname >> sizeTok;
                instrText = rest.substr(q1 + 1, q2 - q1 - 1);
            }

            if (pname.empty() || sizeTok.empty() || instrText.empty()) {
                std::cout << "Usage: screen -c <process_name> <process_memory_size> \"<instructions>\"\n";
                continue;
            }

            long long size = 0;
            bool parsedOk = true;
            try { size = std::stoll(sizeTok); }
            catch (...) { parsedOk = false; }

            if (!parsedOk || !isValidMemSize(size)) {
                std::cout << "Invalid memory allocation.\n";
                continue;
            }

            std::vector<Instruction> program;
            std::string parseError;
            if (!InstructionParser::parse(instrText, program, parseError)) {
                std::cout << "Invalid command: " << parseError << "\n";
                continue;
            }
            if (program.size() < 1 || program.size() > 50) {
                std::cout << "Invalid command: instruction count must be between 1 and 50.\n";
                continue;
            }

            std::lock_guard<std::mutex> lock(allProcessesMutex);
            if (findProcess(pname, allProcesses) != nullptr) {
                std::cout << "Process '" << pname << "' already exists.\n";
            }
            else {
                auto p = createProcessFromInstructions(pname, std::move(program), static_cast<size_t>(size));
                allProcesses.push_back(p);
                scheduler->addProcess(p);
                std::cout << "Process '" << pname << "' created and added to queue.\n";
            }
        }
        else if (line.rfind("screen -r ", 0) == 0) {
            std::string pname = line.substr(10);
            std::shared_ptr<Process> p;
            {
                std::lock_guard<std::mutex> lock(allProcessesMutex);
                p = findProcess(pname, allProcesses);
            }
            if (!p) {
                std::cout << "Process " << pname << " not found.\n";
            }
            else if (p->memoryViolation) {
                std::ostringstream oss;
                oss << std::hex << std::showbase << p->violationAddress;
                std::cout << "Process " << pname << " shut down due to memory access violation error that occurred at "
                    << p->violationTimestamp << ". " << oss.str() << " invalid.\n";
            }
            else if (p->state == ProcessState::FINISHED) {
                std::cout << "Process " << pname << " not found.\n";
            }
            else {
                enterScreen(p);
            }
        }
        else if (line == "scheduler-start") {
            if (batchGenActive.load()) {
                std::cout << "Batch process generation is already running.\n";
            }
            else {
                batchGenActive.store(true);
                batchGenThread = std::thread(batchGenLoop, &allProcesses, &allProcessesMutex);
                std::cout << "Started generating dummy processes every "
                    << config.batchProcessFreq << " tick(s).\n";
            }
        }
        else if (line == "scheduler-stop") {
            if (!batchGenActive.load()) {
                std::cout << "Batch process generation is not running.\n";
            }
            else {
                batchGenActive.store(false);
                if (batchGenThread.joinable()) batchGenThread.join();
                std::cout << "Stopped generating dummy processes.\n";
            }
        }
        else if (line == "report-util") {
            reportUtil();
        }
        else if (line.rfind("gen-mode ", 0) == 0) {
            std::string mode = line.substr(9);
            if (mode == "random") {
                genMode = GenMode::RANDOM;
                std::cout << "Generation mode set to: random.\n";
            }
            else if (mode == "altxy") {
                genMode = GenMode::ALTERNATING_XY;
                std::cout << "Generation mode set to: alternating PRINT/ADD(x).\n";
            }
            else {
                std::cout << "Usage: gen-mode <random|altxy>\n";
            }
        }
        else {
            std::cout << "Unknown command: '" << line << "'. Type 'help' for options.\n";
        }
    }

    return 0;
}