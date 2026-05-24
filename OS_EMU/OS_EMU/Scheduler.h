#pragma once
#include "Process.h"
#include "Config.h"
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

// Design note:
// The scheduler runs on CPU "ticks" rather than wall-clock sleeps for instruction
// pacing. One tick = one pass of the scheduler loop. Each core advances its
// currently-assigned process by one instruction per tick (subject to
// delays-per-exec), then the loop yields briefly so it doesn't spin the CPU at
// 100%. SLEEP instructions decrement a per-process tick counter rather than
// blocking the worker thread, so a sleeping process correctly frees its core.
class Scheduler {
private:
    int numCores;
    SchedulerType type;
    uint32_t quantumCycles;
    uint32_t delaysPerExec;

    std::queue<std::shared_ptr<Process>> readyQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;

    std::vector<std::shared_ptr<Process>> runningProcesses; // indexed by core
    std::vector<uint32_t> quantumUsed;                      // RR: ticks used on current core slice
    std::vector<uint32_t> delayCounters;                    // ticks to wait before next instruction per core
    std::mutex runningMutex;

    std::vector<std::shared_ptr<Process>> finishedProcesses;
    std::mutex finishedMutex;

    std::vector<std::thread> workerThreads;
    std::atomic<bool> running;
    std::atomic<uint64_t> cpuTicks;

public:
    Scheduler(int numCores, SchedulerType type, uint32_t quantumCycles, uint32_t delaysPerExec)
        : numCores(numCores), type(type), quantumCycles(quantumCycles),
        delaysPerExec(delaysPerExec), running(false), cpuTicks(0) {
        runningProcesses.resize(numCores, nullptr);
        quantumUsed.resize(numCores, 0);
        delayCounters.resize(numCores, 0);
    }

    ~Scheduler() {
        stop();
    }

    void start() {
        running.store(true);
        for (int i = 0; i < numCores; i++) {
            workerThreads.emplace_back(&Scheduler::workerLoop, this, i);
        }
    }

    void stop() {
        running.store(false);
        queueCV.notify_all();
        for (auto& t : workerThreads)
            if (t.joinable()) t.join();
        workerThreads.clear();
    }

    void addProcess(std::shared_ptr<Process> process) {
        std::unique_lock<std::mutex> lock(queueMutex);
        readyQueue.push(process);
        lock.unlock();
        queueCV.notify_one();
    }

    std::vector<std::shared_ptr<Process>> getRunningProcesses() {
        std::lock_guard<std::mutex> lock(runningMutex);
        return runningProcesses;
    }

    std::vector<std::shared_ptr<Process>> getReadyProcesses() {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::queue<std::shared_ptr<Process>> copy = readyQueue;
        std::vector<std::shared_ptr<Process>> result;
        while (!copy.empty()) {
            result.push_back(copy.front());
            copy.pop();
        }
        return result;
    }

    std::vector<std::shared_ptr<Process>> getFinishedProcesses() {
        std::lock_guard<std::mutex> lock(finishedMutex);
        return finishedProcesses;
    }

    int getNumCores() const { return numCores; }

    int coresInUse() {
        std::lock_guard<std::mutex> lock(runningMutex);
        int busy = 0;
        for (auto& p : runningProcesses) if (p != nullptr) busy++;
        return busy;
    }

private:
    // Moves a finished/sleeping process off its core slot and either requeues
    // it (sleep) or files it as finished.
    void retireFromCore(int coreId, std::shared_ptr<Process> proc) {
        {
            std::lock_guard<std::mutex> lock(runningMutex);
            runningProcesses[coreId] = nullptr;
            quantumUsed[coreId] = 0;
            delayCounters[coreId] = 0;
        }

        if (proc->isFinished()) {
            proc->state = ProcessState::FINISHED;
            proc->finishedAt = Process::getCurrentTimestamp();
            proc->coreId = -1;
            std::lock_guard<std::mutex> flock(finishedMutex);
            finishedProcesses.push_back(proc);
        }
        else {
            // Either sleeping or RR quantum expired: back to ready queue.
            proc->coreId = -1;
            if (proc->state != ProcessState::WAITING) {
                proc->state = ProcessState::READY;
            }
            std::unique_lock<std::mutex> lock(queueMutex);
            readyQueue.push(proc);
            lock.unlock();
            queueCV.notify_one();
        }
    }

    void workerLoop(int coreId) {
        while (running.load()) {
            std::shared_ptr<Process> proc;

            {
                std::lock_guard<std::mutex> lock(runningMutex);
                proc = runningProcesses[coreId];
            }

            if (!proc) {
                // Pull a new process from the ready queue (skip ones still sleeping).
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCV.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return !readyQueue.empty() || !running.load();
                    });
                if (!running.load()) break;
                if (readyQueue.empty()) continue;

                proc = readyQueue.front();

                // If it's sleeping, tick down its counter and requeue at the back
                // rather than dispatching it. This keeps the core free for others.
                if (proc->state == ProcessState::WAITING) {
                    readyQueue.pop();
                    if (proc->sleepTicksRemaining > 0) proc->sleepTicksRemaining--;
                    if (proc->sleepTicksRemaining == 0) {
                        proc->state = ProcessState::READY;
                    }
                    readyQueue.push(proc);
                    continue;
                }

                readyQueue.pop();
                lock.unlock();

                {
                    std::lock_guard<std::mutex> rlock(runningMutex);
                    proc->state = ProcessState::RUNNING;
                    proc->coreId = coreId;
                    runningProcesses[coreId] = proc;
                    quantumUsed[coreId] = 0;
                    delayCounters[coreId] = 0;
                }
            }

            // Respect delays-per-exec: wait that many ticks between instructions.
            {
                std::lock_guard<std::mutex> rlock(runningMutex);
                if (delayCounters[coreId] > 0) {
                    delayCounters[coreId]--;
                    cpuTicks++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }

            bool didWork = proc->executeOneStep(coreId);
            cpuTicks++;

            if (!didWork || proc->isFinished()) {
                retireFromCore(coreId, proc);
                continue;
            }

            if (proc->state == ProcessState::WAITING) {
                // Hit a SLEEP instruction mid-execution: release the core now.
                retireFromCore(coreId, proc);
                continue;
            }

            bool quantumExpired = false;
            {
                std::lock_guard<std::mutex> rlock(runningMutex);
                delayCounters[coreId] = delaysPerExec;
                if (type == SchedulerType::RR) {
                    quantumUsed[coreId]++;
                    if (quantumUsed[coreId] >= quantumCycles) {
                        quantumExpired = true;
                        runningProcesses[coreId] = nullptr;
                        quantumUsed[coreId] = 0;
                        delayCounters[coreId] = 0;
                    }
                }
            }

            if (quantumExpired) {
                // Preempt: process goes to the back of the ready queue, core is freed.
                proc->state = ProcessState::READY;
                proc->coreId = -1;
                std::unique_lock<std::mutex> lock(queueMutex);
                readyQueue.push(proc);
                lock.unlock();
                queueCV.notify_one();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};