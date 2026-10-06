// CombinedApp.cpp : 合并版
// 功能：
//   1) 启动时自动静默替换桌面所有 .lnk 快捷方式图标和壁纸（来自 AKT 项目）
//   2) 系统托盘后台运行，检测 EasiNote 进程并自动打开浏览器（来自 cpp 项目）
//   3) 开机自启动支持
//   4) icon.ico 和 wallpaper.png 以 RCDATA 资源形式编译进同一个 exe

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <objidl.h>
#include <strsafe.h>
#include <gdiplus.h>

#include <algorithm>
#include <cwctype>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Gdiplus.lib")

// ==================== 常量定义（cpp 项目） ====================
namespace {
constexpr wchar_t kAppName[] = L"课件";
constexpr wchar_t kShortcutName[] = L"课件.lnk";
constexpr wchar_t kTargetUrl[] = L"https://www.bilibili.com/video/BV1gu4y1Z7cu";
constexpr wchar_t kStartupVideoUrl[] = L"https://sr.mihoyo.com";
constexpr UINT kTrayIconId = 1, kTimerId = 1, kTaskbarMessage = WM_APP + 1;
constexpr UINT kMenuStartup = 1001, kMenuOpenSite = 1002, kMenuExit = 1003;

NOTIFYICONDATAW g_tray{};
std::set<DWORD> g_easiNoteProcessIds;
bool g_wasEasiNoteRunning = false;

HINSTANCE g_hInst = NULL;
ULONG_PTR g_gdiplusToken = 0;
}

// ==================== 路径工具（AKT 项目） ====================
static bool GetWorkDir(WCHAR* out, DWORD cch) {
    WCHAR localApp[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localApp)))
        return false;
    StringCchPrintfW(out, cch, L"%s\\ShortcutChanger", localApp);
    CreateDirectoryW(out, NULL);
    return true;
}

static bool GetBackupPath(WCHAR* out, DWORD cch) {
    WCHAR dir[MAX_PATH] = {0};
    if (!GetWorkDir(dir, MAX_PATH)) return false;
    StringCchPrintfW(out, cch, L"%s\\backup.ini", dir);
    return true;
}

// ==================== 释放嵌入资源到磁盘（AKT 项目） ====================
static bool ExtractResource(int resourceId, const WCHAR* destPath) {
    HRSRC hRsrc = FindResourceW(g_hInst, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRsrc) return false;
    DWORD size = SizeofResource(g_hInst, hRsrc);
    HGLOBAL hGlob = LoadResource(g_hInst, hRsrc);
    if (!hGlob || size == 0) return false;
    void* pData = LockResource(hGlob);
    if (!pData) return false;

    HANDLE hFile = CreateFileW(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pData, size, &written, NULL);
    CloseHandle(hFile);
    return ok && written == size;
}

// ==================== PNG 转 BMP（AKT 项目） ====================
static bool PngFileToBmpFile(const WCHAR* pngPath, const WCHAR* bmpPath) {
    using namespace Gdiplus;

    CLSID bmpClsid = {0};
    UINT num = 0, sz = 0;
    GetImageEncodersSize(&num, &sz);
    if (sz == 0) return false;
    ImageCodecInfo* pCodecs = (ImageCodecInfo*)malloc(sz);
    GetImageEncoders(num, sz, pCodecs);
    bool found = false;
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(pCodecs[i].MimeType, L"image/bmp") == 0) {
            bmpClsid = pCodecs[i].Clsid;
            found = true;
            break;
        }
    }
    free(pCodecs);
    if (!found) return false;

    Bitmap* pBmp = Bitmap::FromFile(pngPath, FALSE);
    if (!pBmp || pBmp->GetLastStatus() != Ok) {
        delete pBmp;
        return false;
    }
    Status st = pBmp->Save(bmpPath, &bmpClsid, NULL);
    delete pBmp;
    return st == Ok;
}

// ==================== 枚举所有 .lnk（桌面 + 开始菜单） ====================
static std::vector<std::wstring> EnumerateLnkFiles() {
    std::vector<std::wstring> result;
    WCHAR paths[4][MAX_PATH];
    ZeroMemory(paths, sizeof(paths));

    // 桌面路径（用户 + 公共）
    SHGetFolderPathW(NULL, CSIDL_DESKTOP, NULL, SHGFP_TYPE_CURRENT, paths[0]);
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, paths[1]);
    // 开始菜单路径（用户 + 公共）
    SHGetFolderPathW(NULL, CSIDL_STARTMENU, NULL, SHGFP_TYPE_CURRENT, paths[2]);
    SHGetFolderPathW(NULL, CSIDL_COMMON_STARTMENU, NULL, SHGFP_TYPE_CURRENT, paths[3]);

    for (int d = 0; d < 4; d++) {
        if (paths[d][0] == 0) continue;

        // 递归枚举目录下所有 .lnk 文件（开始菜单有子文件夹）
        std::wstring pattern = std::wstring(paths[d]) + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            WCHAR full[MAX_PATH];
            StringCchPrintfW(full, _countof(full), L"%s\\%s", paths[d], fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 递归遍历子目录
                std::wstring subPattern = std::wstring(full) + L"\\*.lnk";
                WIN32_FIND_DATAW subFd;
                HANDLE hSubFind = FindFirstFileW(subPattern.c_str(), &subFd);
                if (hSubFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        WCHAR subFull[MAX_PATH];
                        StringCchPrintfW(subFull, _countof(subFull), L"%s\\%s", full, subFd.cFileName);
                        result.push_back(subFull);
                    } while (FindNextFileW(hSubFind, &subFd));
                    FindClose(hSubFind);
                }
            } else {
                // 文件，检查是否是 .lnk
                int len = lstrlenW(full);
                if (len > 4 && _wcsicmp(full + len - 4, L".lnk") == 0) {
                    result.push_back(full);
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return result;
}

// ==================== 静默应用主题（AKT 项目 - 无 UI 版本） ====================
static void ApplyThemeSilent() {
    WCHAR workDir[MAX_PATH] = {0};
    WCHAR iconPath[MAX_PATH] = {0};
    WCHAR wallPath[MAX_PATH] = {0};
    WCHAR backupIni[MAX_PATH] = {0};
    GetWorkDir(workDir, MAX_PATH);
    StringCchPrintfW(iconPath, _countof(iconPath), L"%s\\folder.ico", workDir);
    StringCchPrintfW(wallPath, _countof(wallPath), L"%s\\wallpaper.png", workDir);
    GetBackupPath(backupIni, MAX_PATH);

    // 清空旧备份，重新记录
    DeleteFileW(backupIni);

    // 备份当前壁纸
    WCHAR curWall[MAX_PATH] = {0};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, curWall, 0);
    WritePrivateProfileStringW(L"Backup", L"Wallpaper", curWall, backupIni);

    // 释放嵌入的 ico 和 png
    if (!ExtractResource(IDR_ICON_FOLDER, iconPath)) return;
    if (!ExtractResource(IDR_WALLPAPER, wallPath)) return;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::vector<std::wstring> lnks = EnumerateLnkFiles();

    int count = 0;
    for (size_t i = 0; i < lnks.size(); i++) {
        const std::wstring& path = lnks[i];

        IShellLinkW* psl = NULL;
        IPersistFile* ppf = NULL;

        if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, (void**)&psl))) {
            continue;
        }
        if (FAILED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            psl->Release();
            continue;
        }
        if (FAILED(ppf->Load(path.c_str(), STGM_READWRITE))) {
            ppf->Release();
            psl->Release();
            continue;
        }

        // 读取原图标
        WCHAR oldIcon[MAX_PATH] = {0};
        int oldIdx = 0;
        psl->GetIconLocation(oldIcon, MAX_PATH, &oldIdx);

        // 写备份
        WCHAR section[32];
        StringCchPrintfW(section, _countof(section), L"Item%d", count);
        WritePrivateProfileStringW(section, L"Path",  path.c_str(), backupIni);
        WritePrivateProfileStringW(section, L"Icon",  oldIcon,      backupIni);
        WCHAR idxStr[16];
        StringCchPrintfW(idxStr, _countof(idxStr), L"%d", oldIdx);
        WritePrivateProfileStringW(section, L"Index", idxStr, backupIni);

        // 换成新图标
        psl->SetIconLocation(iconPath, 0);
        if (SUCCEEDED(ppf->Save(NULL, TRUE))) {
            count++;
        }

        ppf->Release();
        psl->Release();
    }

    WCHAR cntStr[16];
    StringCchPrintfW(cntStr, _countof(cntStr), L"%d", count);
    WritePrivateProfileStringW(L"Backup", L"Count", cntStr, backupIni);

    // 设置壁纸：Win7 壁纸引擎只认 BMP，先把 PNG 转成 BMP 再设置
    {
        WCHAR bmpPath[MAX_PATH];
        StringCchPrintfW(bmpPath, _countof(bmpPath), L"%s\\wallpaper.bmp", workDir);
        if (PngFileToBmpFile(wallPath, bmpPath)) {
            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)bmpPath,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        }
    }

    // 通知系统刷新图标缓存
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    SHChangeNotify(SHCNE_UPDATEIMAGE, SHCNF_IDLIST, NULL, NULL);

    // Win7 强制刷新：重启 explorer.exe 清除图标缓存
    system("taskkill /f /im explorer.exe & start explorer.exe");

    CoUninitialize();
}

// ==================== 开机自启动功能（注册表方式，更可靠） ====================
std::wstring GetExecutablePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

bool IsStartupEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    wchar_t val[MAX_PATH]{};
    DWORD size = sizeof(val);
    bool exists = RegQueryValueExW(hKey, L"CombinedApp", NULL, NULL, (LPBYTE)val, &size) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return exists;
}

bool SetStartupEnabled(bool enabled) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return false;
    bool ok;
    if (enabled) {
        std::wstring path = GetExecutablePath();
        ok = RegSetValueExW(hKey, L"CombinedApp", 0, REG_SZ, (LPBYTE)path.c_str(),
                            (DWORD)((path.length() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(hKey, L"CombinedApp") == ERROR_SUCCESS || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(hKey);
    return ok;
}

// ==================== EasiNote 进程检测（cpp 项目） ====================
std::set<DWORD> GetEasiNoteProcessIds() {
    std::set<DWORD> processIds;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processIds;

    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        std::wstring name = entry.szExeFile;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.find(L"easinote") != std::wstring::npos) {
            processIds.insert(entry.th32ProcessID);
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return processIds;
}

// 打开 URL 并自动全屏浏览器
void OpenUrlFullscreen(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    // 等待浏览器窗口加载
    Sleep(1500);
    // 获取前台窗口（刚打开的浏览器）
    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        SetForegroundWindow(hwnd);
        // 模拟按 F11 键切换全屏
        keybd_event(VK_F11, 0, 0, 0);
        keybd_event(VK_F11, 0, KEYEVENTF_KEYUP, 0);
    }
}

void OpenSite() { OpenUrlFullscreen(kTargetUrl); }

void OpenRandomStartupSite() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::seed_seq seed{
        static_cast<unsigned int>(counter.LowPart),
        static_cast<unsigned int>(counter.HighPart),
        static_cast<unsigned int>(GetTickCount()),
        static_cast<unsigned int>(GetCurrentProcessId())
    };
    std::mt19937 engine(seed);
    const wchar_t* url = std::uniform_int_distribution<int>(0, 1)(engine) == 0
        ? kTargetUrl : kStartupVideoUrl;
    OpenUrlFullscreen(url);
}

void ShowDetectionNotification() {
    g_tray.uFlags = NIF_INFO;
    lstrcpynW(g_tray.szInfoTitle, kAppName, static_cast<int>(std::size(g_tray.szInfoTitle)));
    lstrcpynW(g_tray.szInfo,
              L"cz",
              static_cast<int>(std::size(g_tray.szInfo)));
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

// ==================== 托盘菜单（cpp 项目） ====================
void ShowMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == kMenuStartup) {
            if (!SetStartupEnabled(!IsStartupEnabled()))
                MessageBoxW(window, L"无法更改登录启动设置。", kAppName, MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wParam) == kMenuOpenSite) { OpenSite(); return 0; }
        if (LOWORD(wParam) == kMenuExit) { DestroyWindow(window); return 0; }
    }
    else if (message == WM_TIMER) {
        const std::set<DWORD> currentIds = GetEasiNoteProcessIds();
        const bool isRunning = !currentIds.empty();
        if (isRunning && !g_wasEasiNoteRunning) {
            OpenSite();
            ShowDetectionNotification();
        }
        g_wasEasiNoteRunning = isRunning;
        g_easiNoteProcessIds = currentIds;
        return 0;
    }
    else if (message == kTaskbarMessage) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowMenu(window);
        return 0;
    }
    else if (message == WM_DESTROY) {
        KillTimer(window, kTimerId);
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ==================== 主入口 ====================
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_hInst = instance;

    // 初始化 GDI+（用于 PNG 转 BMP）
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ====== 第一步：先创建托盘窗口和定时器，确保后台监控功能正常 ======
    constexpr wchar_t className[] = L"EasiNoteWorkHelperWindow";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, className, kAppName, WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = window;
    g_tray.uID = kTrayIconId;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTaskbarMessage;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_tray.szTip, L"akt", static_cast<int>(std::size(g_tray.szTip)));
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    // 设置 EasiNote 检测定时器
    g_easiNoteProcessIds = GetEasiNoteProcessIds();
    g_wasEasiNoteRunning = !g_easiNoteProcessIds.empty();
    SetTimer(window, kTimerId, 250, nullptr);

    // ====== 第二步：执行图标和壁纸替换 ======
    ApplyThemeSilent();

    // ====== 第三步：弹提示和询问自启动 ======
    MessageBoxW(nullptr, L"z", L"嗯", MB_ICONWARNING | MB_OK);

    OpenRandomStartupSite();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (SUCCEEDED(comResult)) CoUninitialize();

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(msg.wParam);
}
