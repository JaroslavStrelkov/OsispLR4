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

struct SharedData {
    int A = 0;
    int B = 0;
    int C = 0;
    int version = 0;
};

SharedData records[RECORD_COUNT];

int main() {
    std::cout << "LR4 / V12\n";
    std::cout << "Records: " << RECORD_COUNT << '\n';

    for (int i = 0; i < RECORD_COUNT; ++i) {
        records[i] = { 0, 0, 0, 0 };
    }
    return 0;
}