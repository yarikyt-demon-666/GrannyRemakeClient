#include <iostream>
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>

DWORD GetPID(const char* procName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 entry{ sizeof(entry) };
        if (Process32First(snap, &entry)) {
            do {
                if (_stricmp(entry.szExeFile, procName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &entry));
        }
        CloseHandle(snap);
    }
    return pid;
}

// Первичный поиск числа в памяти игры
std::vector<uintptr_t> FirstScan(HANDLE hProc, int targetValue) {
    std::vector<uintptr_t> results;
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    uintptr_t currentAddr = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)sysInfo.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi;
    while (currentAddr < maxAddr) {
        if (VirtualQueryEx(hProc, (LPCVOID)currentAddr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE)) {
                std::vector<BYTE> buffer(mbi.RegionSize);
                SIZE_T bytesRead;
                if (ReadProcessMemory(hProc, mbi.BaseAddress, buffer.data(), mbi.RegionSize, &bytesRead)) {
                    for (size_t i = 0; i <= bytesRead - sizeof(int); i += 4) {
                        int val = *(int*)(&buffer[i]);
                        if (val == targetValue) {
                            results.push_back((uintptr_t)mbi.BaseAddress + i);
                        }
                    }
                }
            }
            currentAddr += mbi.RegionSize;
        }
        else {
            break;
        }
    }
    return results;
}

// Отсев найденных адресов при изменении числа в игре
std::vector<uintptr_t> NextScan(HANDLE hProc, const std::vector<uintptr_t>& prevResults, int targetValue) {
    std::vector<uintptr_t> results;
    for (uintptr_t addr : prevResults) {
        int val = 0;
        if (ReadProcessMemory(hProc, (LPCVOID)addr, &val, sizeof(int), nullptr)) {
            if (val == targetValue) {
                results.push_back(addr);
            }
        }
    }
    return results;
}

int main() {
    setlocale(LC_ALL, "Russian");

    const char* processName = "Granny-Win64-Shipping.exe";
    DWORD pid = GetPID(processName);

    if (!pid) {
        std::cout << "Запусти игру (" << processName << ") и перезапусти программу!" << std::endl;
        system("pause");
        return 1;
    }

    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        std::cout << "Нет прав! Запусти от имени администратора." << std::endl;
        system("pause");
        return 1;
    }

    int value = 0;
    std::cout << "Подключено к игре! Введи текущее число (например, 1 для 1-го дня): ";
    std::cin >> value;

    std::cout << "Сканируем память..." << std::endl;
    std::vector<uintptr_t> addresses = FirstScan(hProc, value);
    std::cout << "Найдено совпадений: " << addresses.size() << std::endl;

    while (addresses.size() > 15) {
        std::cout << "Измени значение в игре (например, перейди на День 2) и введи новое число: ";
        std::cin >> value;
        addresses = NextScan(hProc, addresses, value);
        std::cout << "Осталось совпадений: " << addresses.size() << std::endl;
    }

    if (addresses.empty()) {
        std::cout << "Адреса не найдены. Попробуй заново." << std::endl;
        CloseHandle(hProc);
        system("pause");
        return 0;
    }

    int freezeVal = 1;
    std::cout << "Отлично! Какое число заморозить (например, 1): ";
    std::cin >> freezeVal;

    std::cout << "Заморозка включена! Нажми END для выхода." << std::endl;

    while (!(GetAsyncKeyState(VK_END) & 1)) {
        for (uintptr_t addr : addresses) {
            WriteProcessMemory(hProc, (LPVOID)addr, &freezeVal, sizeof(int), nullptr);
        }
        Sleep(100);
    }

    CloseHandle(hProc);
    return 0;
}
