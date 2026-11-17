#include <windows.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <mutex>
#include <string>
#include <fstream>
#include <iomanip>

constexpr int RECORD_COUNT = 16;
constexpr int READERS = 3;
constexpr int WRITERS = 2;
constexpr int OPERATIONS = 200;

struct SharedData {
    int A = 0;
    int B = 0;
    int C = 0;
    int version = 0;
};

SharedData records[RECORD_COUNT];
SRWLOCK globalLock = SRWLOCK_INIT;
SRWLOCK recordLocks[RECORD_COUNT];

std::atomic<int> readOperations{ 0 };
std::atomic<int> writeOperations{ 0 };
std::atomic<int> invariantErrors{ 0 };

bool useRecordLocks = false;

void lockShared(int index) {
    if (useRecordLocks) AcquireSRWLockShared(&recordLocks[index]);
    else                AcquireSRWLockShared(&globalLock);
}
void unlockShared(int index) {
    if (useRecordLocks) ReleaseSRWLockShared(&recordLocks[index]);
    else                ReleaseSRWLockShared(&globalLock);
}
void lockExclusive(int index) {
    if (useRecordLocks) AcquireSRWLockExclusive(&recordLocks[index]);
    else                AcquireSRWLockExclusive(&globalLock);
}
void unlockExclusive(int index) {
    if (useRecordLocks) ReleaseSRWLockExclusive(&recordLocks[index]);
    else                ReleaseSRWLockExclusive(&globalLock);
}

void reader(int id) {
    std::mt19937 gen(1000 + id);
    std::uniform_int_distribution<int> dist(0, RECORD_COUNT - 1);

    for (int i = 0; i < OPERATIONS; ++i) {
        int index = dist(gen);

        lockShared(index);
        SharedData copy = records[index];
        if (copy.B != 2 * copy.A || copy.C != 3 * copy.A) {
            invariantErrors++;
        }
        unlockShared(index);

        readOperations++;
    }
}

void writer(int id) {
    std::mt19937 gen(2000 + id);
    std::uniform_int_distribution<int> dist(0, RECORD_COUNT - 1);

    for (int i = 0; i < OPERATIONS; ++i) {
        int index = dist(gen);

        lockExclusive(index);
        int value = records[index].A + 1;
        records[index].A = value;
        records[index].B = 2 * value;
        records[index].C = 3 * value;
        records[index].version++;
        unlockExclusive(index);

        writeOperations++;
    }
}

int main(int argc, char* argv[]) {
    if (argc > 2 && std::string(argv[1]) == "--lock") {
        useRecordLocks = std::string(argv[2]) == "per-record";
    }

    for (int i = 0; i < RECORD_COUNT; ++i) {
        InitializeSRWLock(&recordLocks[i]);
    }

    std::vector<std::thread> threads;
    for (int i = 0; i < READERS; ++i) threads.emplace_back(reader, i);
    for (int i = 0; i < WRITERS; ++i) threads.emplace_back(writer, i);
    for (auto& thread : threads) thread.join();

    std::cout << "Lock: "
        << (useRecordLocks ? "per-record" : "global") << '\n';
    std::cout << "Read operations: " << readOperations << '\n';
    std::cout << "Write operations: " << writeOperations << '\n';
    std::cout << "Invariant errors: " << invariantErrors << '\n';
    return 0;
}