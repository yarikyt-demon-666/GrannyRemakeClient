#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

const char* PROCESS_NAME = "Granny-Win64-Shipping.exe";

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool g_ShowMenu = true;
DWORD g_Pid = 0;
HANDLE g_hProc = nullptr;
std::string g_StatusText = "Ожидание...";

// 1. Вкладка: Дни
int g_ScanValue = 1, g_FreezeValue = 1;
bool g_FreezeActive = false;
std::vector<uintptr_t> g_DayAddresses;

// 2. Вкладка: Таймер оглушения бабки + Чек-лист побега
bool g_StunRunning = false;
float g_StunDuration = 60.0f;
ULONGLONG g_StunStartTime = 0;
bool g_StunAlertPlayed = false;
bool g_ItemHammer = false, g_ItemPliers = false, g_ItemMasterKey = false;
bool g_ItemSafeKey = false, g_ItemPadlockKey = false, g_ItemCode = false;

// 3. Вкладка: Макросы, Автокликер и Залипание
bool g_AutoClickLMB = false;
int g_ClickDelayMs = 50;
ULONGLONG g_LastClickTime = 0;
bool g_AutoSpamE = false;
bool g_AutoWalkW = false;
bool g_AutoCrouchCtrl = false;

// 4. Вкладка: Визуал, Свет и Прицел
bool g_Fullbright = false;
float g_GammaLevel = 2.0f;
std::vector<WORD> g_OriginalGamma(768, 0);
bool g_GammaSaved = false;

bool g_Crosshair = true;
int g_CrosshairType = 0; // 0 = Крестик, 1 = Точка, 2 = Круг
float g_CrosshairSize = 7.0f;
float g_CrosshairR = 0.0f, g_CrosshairG = 1.0f, g_CrosshairB = 0.4f;
bool g_Watermark = true;
bool g_CenterDotOnStun = true;

void SetScreenGamma(float gamma) {
    HDC hDC = GetDC(nullptr);
    if (!hDC) return;
    if (!g_GammaSaved) {
        if (GetDeviceGammaRamp(hDC, g_OriginalGamma.data())) g_GammaSaved = true;
    }
    std::vector<WORD> ramp(768, 0);
    for (int i = 0; i < 256; i++) {
        double val = std::pow(i / 255.0, 1.0 / gamma) * 65535.0;
        if (val > 65535.0) val = 65535.0;
        WORD wVal = (WORD)val;
        ramp.at(i) = wVal;
        ramp.at(256 + i) = wVal;
        ramp.at(512 + i) = wVal;
    }
    SetDeviceGammaRamp(hDC, ramp.data());
    ReleaseDC(nullptr, hDC);
}

void RestoreScreenGamma() {
    if (!g_GammaSaved) return;
    HDC hDC = GetDC(nullptr);
    if (hDC) {
        SetDeviceGammaRamp(hDC, g_OriginalGamma.data());
        ReleaseDC(nullptr, hDC);
    }
}

void TapKey(WORD vk) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    SendInput(1, &in, sizeof(INPUT));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void ClickMouseLMB() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &in, sizeof(INPUT));
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

DWORD GetPID(const char* procName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 entry{ sizeof(entry) };
        if (Process32First(snap, &entry)) {
            do {
                if (_stricmp(entry.szExeFile, procName) == 0) { pid = entry.th32ProcessID; break; }
            } while (Process32Next(snap, &entry));
        }
        CloseHandle(snap);
    }
    return pid;
}

HWND FindGameWindow(DWORD pid) {
    struct EnumData { DWORD pid; HWND hwnd; } data{ pid, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = (EnumData*)lParam;
        DWORD winPid = 0; GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid == d->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) { d->hwnd = hwnd; return FALSE; }
        return TRUE;
        }, (LPARAM)&data);
    return data.hwnd;
}

void FirstScanInt(int val) {
    g_DayAddresses.clear();
    if (!g_hProc) return;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t cur = (uintptr_t)si.lpMinimumApplicationAddress, max = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (cur < max && VirtualQueryEx(g_hProc, (LPCVOID)cur, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE)) {
            std::vector<BYTE> buf(mbi.RegionSize); SIZE_T read = 0;
            if (ReadProcessMemory(g_hProc, mbi.BaseAddress, buf.data(), mbi.RegionSize, &read)) {
                for (size_t i = 0; i + 4 <= read; i += 4)
                    if (*(int*)(&buf.at(i)) == val) g_DayAddresses.push_back((uintptr_t)mbi.BaseAddress + i);
            }
        }
        cur += mbi.RegionSize;
    }
    g_StatusText = "Найдено: " + std::to_string(g_DayAddresses.size());
}

void NextScanInt(int val) {
    if (!g_hProc || g_DayAddresses.empty()) return;
    std::vector<uintptr_t> res;
    for (uintptr_t a : g_DayAddresses) {
        int v = 0;
        if (ReadProcessMemory(g_hProc, (LPCVOID)a, &v, 4, nullptr) && v == val) res.push_back(a);
    }
    g_DayAddresses = res;
    g_StatusText = "Осталось: " + std::to_string(g_DayAddresses.size());
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    std::vector<D3D_FEATURE_LEVEL> fla = { D3D_FEATURE_LEVEL_11_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fla.data(), 1, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext) != S_OK) return false;
    ID3D11Texture2D* bb = nullptr; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRenderTargetView); bb->Release(); }
    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void SetOverlayClickable(HWND hwnd, bool clickable) {
    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (clickable) {
        SetWindowLong(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
        SetForegroundWindow(hwnd);
    }
    else {
        SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        if (g_Pid) { HWND gh = FindGameWindow(g_Pid); if (gh) SetForegroundWindow(gh); }
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, hInst, nullptr, nullptr, nullptr, nullptr, "GrannyOverlay", nullptr };
    RegisterClassEx(&wc);
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, "Overlay", WS_POPUP, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    MARGINS m = { -1 }; DwmExtendFrameIntoClientArea(hwnd, &m);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);

    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClass(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    if (GetFileAttributesA("C:\\Windows\\Fonts\\arial.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.65f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.55f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.12f, 0.15f, 1.00f);


    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || (GetAsyncKeyState(VK_END) & 1)) break;

        if (GetAsyncKeyState(VK_INSERT) & 1) { g_ShowMenu = !g_ShowMenu; SetOverlayClickable(hwnd, g_ShowMenu); }

        // F2 — Ночное видение
        if (GetAsyncKeyState(VK_F2) & 1) {
            g_Fullbright = !g_Fullbright;
            if (g_Fullbright) SetScreenGamma(g_GammaLevel);
            else RestoreScreenGamma();
        }

        // F3 — Запуск таймера оглушения бабки
        if (GetAsyncKeyState(VK_F3) & 1) {
            g_StunRunning = true;
            g_StunStartTime = GetTickCount64();
            g_StunAlertPlayed = false;
            Beep(600, 120);
        }

        // F4 — Автоходьба (залипание W)
        if (GetAsyncKeyState(VK_F4) & 1) {
            g_AutoWalkW = !g_AutoWalkW;
            INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = 'W';
            if (!g_AutoWalkW) in.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
        }

        // Макросы при закрытом меню
        ULONGLONG now = GetTickCount64();
        if (!g_ShowMenu && (now - g_LastClickTime >= (ULONGLONG)g_ClickDelayMs)) {
            if (g_AutoClickLMB && (GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) {
                ClickMouseLMB();
                g_LastClickTime = now;
            }
            if (g_AutoSpamE && (GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) {
                TapKey('E');
                g_LastClickTime = now;
            }
        }

        // Заморозка дней
        if (g_FreezeActive && g_hProc && !g_DayAddresses.empty())
            for (uintptr_t a : g_DayAddresses) WriteProcessMemory(g_hProc, (LPVOID)a, &g_FreezeValue, 4, nullptr);

        // Расчёт таймера бабки
        float stunRemaining = 0.0f;
        if (g_StunRunning) {
            float elapsed = (now - g_StunStartTime) / 1000.0f;
            stunRemaining = g_StunDuration - elapsed;
            if (stunRemaining <= 10.0f && stunRemaining > 0.0f && !g_StunAlertPlayed) {
                g_StunAlertPlayed = true;
                Beep(900, 200);
            }
            if (stunRemaining <= 0.0f) {
                g_StunRunning = false;
                stunRemaining = 0.0f;
                Beep(400, 300);
            }
        }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImDrawList* bg = ImGui::GetBackgroundDrawList();

        // Вотермарка сверху слева
        if (g_Watermark) {
            std::string wm = "Granny Client v4.0 | [INS] Меню | [F2] Свет: " + std::string(g_Fullbright ? "ВКЛ" : "ВЫКЛ");
            if (g_StunRunning) {
                wm += " | БАБКА ВСТАНЕТ ЧЕРЕЗ: " + std::to_string((int)stunRemaining) + " сек!";
            }
            float boxWidth = ImGui::CalcTextSize(wm.c_str()).x + 20.0f;
            bg->AddRectFilled(ImVec2(12, 12), ImVec2(12 + boxWidth, 42), IM_COL32(18, 18, 22, 215), 6.0f);
            bg->AddRect(ImVec2(12, 12), ImVec2(12 + boxWidth, 42), g_StunRunning ? IM_COL32(255, 180, 0, 255) : IM_COL32(190, 40, 50, 255), 6.0f);
            bg->AddText(ImVec2(22, 19), IM_COL32(255, 255, 255, 255), wm.c_str());
        }

        // Кастомный прицел
        if (g_Crosshair) {
            float cx = w / 2.0f, cy = h / 2.0f;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(g_CrosshairR, g_CrosshairG, g_CrosshairB, 1.0f));
            if (g_CrosshairType == 0) {
                bg->AddLine(ImVec2(cx - g_CrosshairSize, cy), ImVec2(cx + g_CrosshairSize, cy), col, 2.0f);
                bg->AddLine(ImVec2(cx, cy - g_CrosshairSize), ImVec2(cx, cy + g_CrosshairSize), col, 2.0f);
            }
            else if (g_CrosshairType == 1) {
                bg->AddCircleFilled(ImVec2(cx, cy), g_CrosshairSize * 0.5f, col);
            }
            else {
                bg->AddCircle(ImVec2(cx, cy), g_CrosshairSize, col, 24, 2.0f);
            }
        }

        if (g_ShowMenu) {
            ImGui::SetNextWindowSize(ImVec2(480, 390), ImGuiCond_FirstUseEver);
            ImGui::Begin("Granny Remake Ultimate v4.0 [INSERT]", &g_ShowMenu);

            if (ImGui::Button("Подключиться к игре")) {
                g_Pid = GetPID(PROCESS_NAME);
                if (g_Pid) {
                    if (g_hProc) CloseHandle(g_hProc);
                    g_hProc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, g_Pid);
                    g_StatusText = g_hProc ? ("PID: " + std::to_string(g_Pid)) : "Запусти от админа!";
                }
                else g_StatusText = "Игра не найдена!";
            }
            ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", g_StatusText.c_str());
            ImGui::Separator();

            if (ImGui::BeginTabBar("MainTabs")) {
                if (ImGui::BeginTabItem("Дни")) {
                    ImGui::InputInt("Текущий день", &g_ScanValue);
                    if (ImGui::Button("Поиск (День 1)")) FirstScanInt(g_ScanValue);
                    ImGui::SameLine(); if (ImGui::Button("Отсеять (День 2+)")) NextScanInt(g_ScanValue);
                    ImGui::Separator();
                    ImGui::InputInt("Заморозить день", &g_FreezeValue);
                    ImGui::Checkbox("Бесконечные дни", &g_FreezeActive);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Таймер и Предметы")) {
                    ImGui::SliderFloat("Время оглушения (сек)", &g_StunDuration, 30.0f, 150.0f);
                    if (ImGui::Button("Бабка вырублена [F3]")) {
                        g_StunRunning = true; g_StunStartTime = GetTickCount64(); g_StunAlertPlayed = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Сбросить таймер")) g_StunRunning = false;

                    if (g_StunRunning) {
                        ImGui::ProgressBar(stunRemaining / g_StunDuration, ImVec2(-1, 20));
                    }
                    ImGui::Separator();
                    ImGui::Text("Чек-лист предметов для побега:");
                    ImGui::Checkbox("Молоток", &g_ItemHammer); ImGui::SameLine(160); ImGui::Checkbox("Кусачки", &g_ItemPliers);
                    ImGui::Checkbox("Мастер-ключ", &g_ItemMasterKey); ImGui::SameLine(160); ImGui::Checkbox("Ключ от сейфа", &g_ItemSafeKey);
                    ImGui::Checkbox("Ключ от замка", &g_ItemPadlockKey); ImGui::SameLine(160); ImGui::Checkbox("Код от двери", &g_ItemCode);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Макросы")) {
                    ImGui::SliderInt("Задержка клика (мс)", &g_ClickDelayMs, 20, 200);
                    ImGui::Checkbox("Автокликер ЛКМ (удерж. Боковую кнопку 1)", &g_AutoClickLMB);
                    ImGui::Checkbox("Турбо-спам [E] (удерж. Боковую кнопку 2)", &g_AutoSpamE);
                    ImGui::Separator();
                    if (ImGui::Checkbox("Автобег вперёд [F4]", &g_AutoWalkW)) {
                        INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = 'W';
                        if (!g_AutoWalkW) in.ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(1, &in, sizeof(INPUT));
                    }
                    if (ImGui::Checkbox("Авто-присед (зажать Ctrl)", &g_AutoCrouchCtrl)) {
                        INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = VK_LCONTROL;
                        if (!g_AutoCrouchCtrl) in.ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(1, &in, sizeof(INPUT));
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Визуал")) {
                    if (ImGui::Checkbox("Ночное видение [F2]", &g_Fullbright)) {
                        if (g_Fullbright) SetScreenGamma(g_GammaLevel);
                        else RestoreScreenGamma();
                    }
                    if (ImGui::SliderFloat("Яркость света", &g_GammaLevel, 1.0f, 3.5f)) {
                        if (g_Fullbright) SetScreenGamma(g_GammaLevel);
                    }
                    ImGui::Separator();
                    ImGui::Checkbox("Кастомный прицел", &g_Crosshair);
                    ImGui::Combo("Форма прицела", &g_CrosshairType, "Крестик\0Точка\0Круг\0");
                    ImGui::SliderFloat("Размер прицела", &g_CrosshairSize, 2.0f, 20.0f);
                    ImGui::SliderFloat("Красный (R)", &g_CrosshairR, 0.0f, 1.0f);
                    ImGui::SliderFloat("Зелёный (G)", &g_CrosshairG, 0.0f, 1.0f);
                    ImGui::SliderFloat("Синий (B)", &g_CrosshairB, 0.0f, 1.0f);
                    ImGui::Checkbox("Вотермарка сверху", &g_Watermark);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (ImGui::Button("Выход [END]")) done = true;
            ImGui::End();
            if (!g_ShowMenu) SetOverlayClickable(hwnd, false);
        }
        ImGui::Render();
        std::vector cc = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc.data());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
}
RestoreScreenGamma();
if (g_hProc) CloseHandle(g_hProc);
ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
CleanupDeviceD3D(); DestroyWindow(hwnd); UnregisterClass(wc.lpszClassName, wc.hInstance);
return 0;
}