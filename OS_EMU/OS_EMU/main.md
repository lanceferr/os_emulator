// this is main.cpp
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

static int pidCounter = 1;
static Config config;
static std::unique_ptr<Scheduler> scheduler;
static InstructionGenerator insGen;

static std::atomic<bool> batchGenActive(false);
static std::thread batchGenThread;
static std::atomic<int> batchPidSeq(1);

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

std::shared_ptr<Process> createProcess(const std::string& name, int numInstructions) {
    auto program = insGen.generate(name, numInstructions);
    int flatTotal = countFlatInstructions(program);
    auto p = std::make_shared<Process>(name, pidCounter++, std::move(program), flatTotal);
    return p;
}

void printHelp() {
    std::cout << "Available commands:\n";
    std::cout << "  initialize              - Load config.txt and start the scheduler\n";
    std::cout << "  screen -ls              - List all running and finished processes\n";
    std::cout << "  screen -s <name>        - Create a new process\n";
    std::cout << "  screen -r <name>        - Reattach to an existing process\n";
    std::cout << "  scheduler-start         - Begin generating dummy processes\n";
    std::cout << "  scheduler-stop          - Stop generating dummy processes\n";
    std::cout << "  report-util             - Show CPU utilization, save to csopesy-log.txt\n";
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
            oss << std::left << std::setw(12) << p->name
                << "  " << fmtTime(p->createdAt)
                << "    Finished"
                << "    " << p->totalInstructions
                << " / " << p->totalInstructions << "\n";
        }
    }
    oss << "----------------------------------------------\n";
    return oss.str();
}

void printScreenLs() {
    std::cout << buildUtilReport();
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
    if (p->state == ProcessState::FINISHED) {
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
                std::cout << "(Process finished. Returning to main menu.)\n";
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

        auto p = createProcess(pname, numInstr);
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
        else if (line.rfind("screen -s ", 0) == 0) {
            std::string pname = line.substr(10);
            if (pname.empty()) {
                std::cout << "Usage: screen -s <process_name>\n";
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
                    auto p = createProcess(pname, numInstr);
                    allProcesses.push_back(p);
                    scheduler->addProcess(p);
                    std::cout << "Process '" << pname << "' created and added to queue.\n";
                }
            }
        }
        else if (line.rfind("screen -r ", 0) == 0) {
            std::string pname = line.substr(10);
            std::shared_ptr<Process> p;
            {
                std::lock_guard<std::mutex> lock(allProcessesMutex);
                p = findProcess(pname, allProcesses);
            }
            if (!p || p->state == ProcessState::FINISHED) {
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
        else {
            std::cout << "Unknown command: '" << line << "'. Type 'help' for options.\n";
        }
    }

    return 0;
}