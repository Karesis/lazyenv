#include <windows.h>
#include <wininet.h>
#include <shlobj.h> // For IsUserAnAdmin, ShellExecuteEx
#include <stdio.h>
#include "cJSON.h" // Include the cJSON header

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib") // For ShellExecuteEx

// Define control IDs & custom messages
#define IDC_STATUS_LABEL 101
#define IDC_START_BUTTON 102
#define IDC_VERSION_COMBO 103
#define WM_APP_VERSIONS_LOADED (WM_APP + 1)

// Global handles and data
HWND hStatusLabel;
HWND hStartButton;
HWND hVersionCombo;
char g_selectedVersion[32] = {0}; // To store the chosen python version

// Function Prototypes
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
BOOL IsElevated();
void RelaunchAsAdmin();
DWORD WINAPI FetchVersionsThread(LPVOID lpParam);
DWORD WINAPI InstallationThread(LPVOID lpParam);
BOOL ExecuteAndCaptureOutput(char* command, char* output, DWORD outputSize, HWND hStatus); 
// (Other utility function prototypes are assumed to be the same)
BOOL DownloadFile(const char* url, const char* savePath, HWND hStatus);
BOOL ExecuteCommand(char* command, HWND hStatus);

// --- Admin Rights and UAC Handling ---

BOOL IsElevated() {
    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    TOKEN_ELEVATION elevation;
    DWORD dwSize;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            fIsElevated = elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
    return fIsElevated;
}

void RelaunchAsAdmin() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath))) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.hwnd = NULL;
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }
}


// --- Network and Version Fetching Thread ---

DWORD WINAPI FetchVersionsThread(LPVOID lpParam) {
    HWND hwnd = (HWND)lpParam;
    HINTERNET hInternet, hConnect;
    char buffer[8192];
    DWORD bytesRead;
    char* jsonResponse = NULL;
    DWORD totalSize = 0;

    hInternet = InternetOpen("PythonVersionFetcher", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        PostMessage(hwnd, WM_APP_VERSIONS_LOADED, (WPARAM)NULL, (LPARAM)-1);
        return 1;
    }

    // Use HTTPS for GitHub API
    hConnect = InternetOpenUrl(hInternet, 
        "https://api.github.com/repos/python/cpython/tags", 
        "User-Agent: C-Installer-App\r\n", -1L, 
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        PostMessage(hwnd, WM_APP_VERSIONS_LOADED, (WPARAM)NULL, (LPARAM)-1);
        return 1;
    }

    // Read the response into a dynamic buffer
    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        char* temp = realloc(jsonResponse, totalSize + bytesRead + 1);
        if (temp == NULL) { /* handle error */ free(jsonResponse); break; }
        jsonResponse = temp;
        memcpy(jsonResponse + totalSize, buffer, bytesRead);
        totalSize += bytesRead;
    }
    if(jsonResponse) jsonResponse[totalSize] = '\0';
    
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    // Post the raw JSON string to the main thread for parsing
    PostMessage(hwnd, WM_APP_VERSIONS_LOADED, (WPARAM)jsonResponse, 0);
    return 0;
}


// --- Installation Thread ---

DWORD WINAPI InstallationThread(LPVOID lpParam) {
    char pythonUrl[256];
    char localPath[] = "python_installer.exe";

    // Step 1: Construct URL and Download
    sprintf_s(pythonUrl, sizeof(pythonUrl), "https://www.python.org/ftp/python/%s/python-%s-amd64.exe", g_selectedVersion, g_selectedVersion);
    
    if (!DownloadFile(pythonUrl, localPath, hStatusLabel)) {
        SetWindowText(hStatusLabel, "Error: Download failed! Check version number or network.");
        EnableWindow(hStartButton, TRUE);
        return 1;
    }

    // Step 2: Install
    char installCmd[512];
    sprintf_s(installCmd, sizeof(installCmd), "%s /quiet InstallAllUsers=0 PrependPath=1", localPath);
    if (!ExecuteCommand(installCmd, hStatusLabel)) {
        SetWindowText(hStatusLabel, "Error: Failed to execute installation command!");
        EnableWindow(hStartButton, TRUE);
        return 1;
    }

    // Step 3: Verify Installation
    Sleep(3000); // Wait a moment for system PATH to potentially update

    char verifyCmd[] = "python --version";
    char result[4096]; // Buffer to store the command output

    if (ExecuteAndCaptureOutput(verifyCmd, result, sizeof(result), hStatusLabel)) {
        // Check if the output contains the word "Python"
        if (strstr(result, "Python")) {
            char successMsg[512];
            // Clean up the result string (remove trailing newlines) for better display
            strtok(result, "\r\n"); 
            sprintf_s(successMsg, sizeof(successMsg), "Success! Detected version: %s", result);
            SetWindowText(hStatusLabel, successMsg);
        } else {
            // Command ran, but output was not what we expected
            SetWindowText(hStatusLabel, "Verification Failed: Python installed but not in PATH. Please restart your PC.");
        }
    } else {
        // The command itself failed to execute
        SetWindowText(hStatusLabel, "Error: Verification command failed to run. Please restart your PC.");
    }

    // Re-enable the button once everything is finished
    EnableWindow(hStartButton, TRUE); 
    return 0;
}

// --- Excute and capture output  --

BOOL ExecuteAndCaptureOutput(char* command, char* output, DWORD outputSize, HWND hStatus) {
    SetWindowText(hStatus, "Verifying installation...");
    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &sa, 0)) return FALSE;
    if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) return FALSE;

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hChildStd_OUT_Wr;
    si.hStdOutput = hChildStd_OUT_Wr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcess(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        return FALSE;
    }

    CloseHandle(hChildStd_OUT_Wr); // Parent doesn't need the write end

    DWORD dwRead;
    CHAR chBuf[4096];
    output[0] = '\0';
    while (ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead != 0) {
        chBuf[dwRead] = '\0';
        strcat_s(output, outputSize, chBuf);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hChildStd_OUT_Rd);
    return TRUE;
}

// --- GUI and Main Application Logic ---

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // UAC Check: If not admin, relaunch and exit.
    if (!IsElevated()) {
        RelaunchAsAdmin();
        return 0;
    }
    
    // The rest of the application starts only if it's running as admin.
    const char CLASS_NAME[] = "PythonInstallerWindowClass";
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, "Advanced Python Installer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 450, 220,
        NULL, NULL, hInstance, NULL);

    if (hwnd == NULL) return 0;

    ShowWindow(hwnd, nCmdShow);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            hStatusLabel = CreateWindow("STATIC", "Fetching available Python versions...",
                WS_VISIBLE | WS_CHILD | SS_LEFT,
                20, 20, 400, 40, hwnd, (HMENU)IDC_STATUS_LABEL, NULL, NULL);

            CreateWindow("STATIC", "Select Version:", WS_VISIBLE | WS_CHILD,
                20, 70, 100, 25, hwnd, NULL, NULL, NULL);
            
            hVersionCombo = CreateWindow("COMBOBOX", NULL,
                WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                130, 70, 150, 100, hwnd, (HMENU)IDC_VERSION_COMBO, NULL, NULL);

            hStartButton = CreateWindow("BUTTON", "Start Installation",
                WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                145, 120, 150, 40, hwnd, (HMENU)IDC_START_BUTTON, NULL, NULL);
            
            EnableWindow(hStartButton, FALSE); // Disable until versions are loaded

            // Start thread to fetch versions
            CreateThread(NULL, 0, FetchVersionsThread, hwnd, 0, NULL);
            break;
        }

        case WM_APP_VERSIONS_LOADED: {
            char* jsonString = (char*)wParam;
            if (lParam == -1 || jsonString == NULL) {
                SetWindowText(hStatusLabel, "Error: Failed to fetch Python versions. Check network.");
                free(jsonString);
                break;
            }

            cJSON* root = cJSON_Parse(jsonString);
            if (root) {
                SetWindowText(hStatusLabel, "Status: Ready. Please select a version.");
                cJSON* tag;
                int count = 0;
                cJSON_ArrayForEach(tag, root) {
                    if (cJSON_IsObject(tag)) {
                        cJSON* name = cJSON_GetObjectItem(tag, "name");
                        if (cJSON_IsString(name)) {
                            // Filter out pre-releases
                            if (!strstr(name->valuestring, "a") && !strstr(name->valuestring, "b") && !strstr(name->valuestring, "rc")) {
                                // Remove the leading 'v'
                                char* version = name->valuestring + 1;
                                SendMessage(hVersionCombo, CB_ADDSTRING, 0, (LPARAM)version);
                                count++;
                            }
                        }
                    }
                }
                cJSON_Delete(root);
                if (count > 0) {
                    SendMessage(hVersionCombo, CB_SETCURSEL, 0, 0); // Select the first (latest) item
                    EnableWindow(hStartButton, TRUE);
                } else {
                     SetWindowText(hStatusLabel, "No stable versions found after filtering.");
                }
            } else {
                 SetWindowText(hStatusLabel, "Error: Failed to parse version data.");
            }
            free(jsonString); // Free the memory allocated in the thread
            break;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_START_BUTTON) {
                int index = SendMessage(hVersionCombo, CB_GETCURSEL, 0, 0);
                if (index != CB_ERR) {
                    SendMessage(hVersionCombo, CB_GETLBTEXT, index, (LPARAM)g_selectedVersion);
                    EnableWindow(hStartButton, FALSE);
                    CreateThread(NULL, 0, InstallationThread, NULL, 0, NULL);
                } else {
                    SetWindowText(hStatusLabel, "Please select a version first.");
                }
            }
            break;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

BOOL DownloadFile(const char* url, const char* savePath, HWND hStatus) {
    SetWindowText(hStatus, "Downloading Python installer...");
    HINTERNET hInternet, hConnect;
    char buffer[4096];
    DWORD bytesRead;
    FILE* pFile = NULL;

    hInternet = InternetOpen("MyPythonDownloader", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hInternet == NULL) return FALSE;

    hConnect = InternetOpenUrl(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (hConnect == NULL) {
        InternetCloseHandle(hInternet);
        return FALSE;
    }
    
    // Open file in binary write mode
    if (fopen_s(&pFile, savePath, "wb") != 0 || pFile == NULL) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        fwrite(buffer, 1, bytesRead, pFile);
    }

    fclose(pFile);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    SetWindowText(hStatus, "Download complete.");
    return TRUE;
}

BOOL ExecuteCommand(char* command, HWND hStatus) {
    SetWindowText(hStatus, "Silently installing Python... (This may take 1-2 minutes)");
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcess(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SetWindowText(hStatus, "Installation process has finished.");
    return TRUE;
}