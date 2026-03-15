#include <Windows.h>
#include <windowsx.h>
#include <string>
#include <vector>

#define VK_Z_KEY 0x5A
#define VK_X_KEY 0x58
// These keys are used to send windows to tray
#define TRAY_KEY VK_Z_KEY
#define EXIT_KEY VK_X_KEY
#define MOD_KEY (MOD_WIN | MOD_SHIFT)

#define HOTKEY_MINIMIZE 0
#define HOTKEY_EXIT 1

#define WM_ICON 0x1C0A
#define WM_TRAYMIN (WM_USER + 1)
#define MAXIMUM_WINDOWS 100

// Stores hidden window record.
typedef struct HIDDEN_WINDOW {
  NOTIFYICONDATA icon;
  HWND window;
} HIDDEN_WINDOW;

// Current execution context
typedef struct TRCONTEXT {
  HWND mainWindow;
  HIDDEN_WINDOW icons[MAXIMUM_WINDOWS];
  int iconIndex; // How many windows are currently hidden
  UINT nextIconId; // Monotonic counter for unique icon IDs (x64-safe)
} TRCONTEXT;

HANDLE saveFile;

// Globals for low-level mouse hook (callbacks can't receive user data)
HWND g_mainWindow = NULL;
HHOOK g_mouseHook = NULL;

// Registered message ID for Explorer restart notification
UINT WM_TASKBARCREATED = 0;

// Saves our hidden windows so they can be restored in case
// of crashing.
void save(const TRCONTEXT *context) {
  DWORD numbytes;
  // Truncate file
  SetFilePointer(saveFile, 0, NULL, FILE_BEGIN);
  SetEndOfFile(saveFile);
  if (!context->iconIndex) {
    return;
  }
  for (int i = 0; i < context->iconIndex; i++)
  {
    if (context->icons[i].window) {
      std::string str;
      str = std::to_string((LONG_PTR)context->icons[i].window);
      str += ',';
      const char *handleString = str.c_str();
      WriteFile(saveFile, handleString, strlen(handleString), &numbytes, NULL);
    }
  }
}

// Restores a window
void showWindow(TRCONTEXT *context, LPARAM lParam) {
  for (int i = 0; i < context->iconIndex; i++)
  {
    if (context->icons[i].icon.uID == HIWORD(lParam)) {
      ShowWindow(context->icons[i].window, SW_SHOW);
      Shell_NotifyIcon(NIM_DELETE, &context->icons[i].icon);
      SetForegroundWindow(context->icons[i].window);
      context->icons[i] = {};
      std::vector<HIDDEN_WINDOW> temp = std::vector<HIDDEN_WINDOW>(context->iconIndex);
      // Restructure array so there are no holes
      for (int j = 0, x = 0; j < context->iconIndex; j++)
      {
        if (context->icons[j].window) {
          temp[x] = context->icons[j];
          x++;
        }
      }
      memcpy_s(context->icons, sizeof(context->icons), &temp.front(), sizeof(HIDDEN_WINDOW)*context->iconIndex);
      context->iconIndex--;
      save(context);
      break;
    }
  }
}

// Minimizes the current window to tray.
// Uses currently focused window unless supplied a handle as the argument.
void minimizeToTray(TRCONTEXT *context, LONG_PTR restoreWindow) {
  // Taskbar and desktop windows are restricted from hiding.
  const char restrictWins[][14] = { {"WorkerW"}, {"Shell_TrayWnd"} };

  HWND currWin = 0;
  if (!restoreWindow) {
    currWin = GetForegroundWindow();
  }
  else {
    currWin = reinterpret_cast<HWND>(restoreWindow);
  }

  if (!currWin) {
    return;
  }

  char className[256];
  if (!GetClassName(currWin, className, 256)) {
    return;
  }
  else {
    for (int i = 0; i < sizeof(restrictWins) / sizeof(*restrictWins); i++)
    {
      if (strcmp(restrictWins[i], className) == 0) {
        return;
      }
    }
  }
  if (context->iconIndex == MAXIMUM_WINDOWS) {
    MessageBox(NULL, "Error! Too many hidden windows. Please unhide some.", "Traymond", MB_OK | MB_ICONERROR);
    return;
  }

  // Check if this window is already minimized to tray
  for (int i = 0; i < context->iconIndex; i++) {
    if (context->icons[i].window == currWin) {
      return; // Already in tray, don't add again
    }
  }

  // Extract icon from the process executable — this is the most reliable
  // source, especially for Chromium/Electron windows whose window icon
  // handles can be stale or blank.
  HICON exeIcon = NULL;
  bool ownExeIcon = false;
  {
    DWORD pid = 0;
    GetWindowThreadProcessId(currWin, &pid);
    if (pid) {
      HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (proc) {
        char exePath[MAX_PATH];
        DWORD pathLen = MAX_PATH;
        if (QueryFullProcessImageName(proc, 0, exePath, &pathLen)) {
          exeIcon = ExtractIcon(GetModuleHandle(NULL), exePath, 0);
          if (exeIcon == (HICON)1) exeIcon = NULL;
          else if (exeIcon) ownExeIcon = true;
        }
        CloseHandle(proc);
      }
    }
  }

  // Try window icon handles, fall back to exe icon
  ULONG_PTR icon = GetClassLongPtr(currWin, GCLP_HICONSM);
  if (!icon) icon = SendMessage(currWin, WM_GETICON, 2, NULL);  // ICON_SMALL2
  if (!icon) icon = SendMessage(currWin, WM_GETICON, 0, NULL);  // ICON_SMALL
  if (!icon) icon = SendMessage(currWin, WM_GETICON, 1, NULL);  // ICON_BIG
  if (!icon && exeIcon) icon = (ULONG_PTR)exeIcon;
  if (!icon) {
    if (ownExeIcon && exeIcon) DestroyIcon(exeIcon);
    return;
  }

  // For Chromium/Electron windows, prefer the exe icon — window icon
  // handles often render as blank in the tray.
  char clsName[64];
  if (GetClassName(currWin, clsName, sizeof(clsName)) &&
      strcmp(clsName, "Chrome_WidgetWin_1") == 0 && exeIcon) {
    icon = (ULONG_PTR)exeIcon;
  }

  NOTIFYICONDATA nid = {};
  nid.cbSize = sizeof(NOTIFYICONDATA);
  nid.hWnd = context->mainWindow;
  nid.hIcon = (HICON)icon;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uVersion = NOTIFYICON_VERSION_4;
  // Monotonic counter for unique icon IDs. Must stay in 16-bit range because
  // NOTIFYICON_VERSION_4 callbacks pack the ID in HIWORD(lParam).
  context->nextIconId++;
  if (context->nextIconId > 0xFFFE) context->nextIconId = 1; // wrap, skip 0
  nid.uID = context->nextIconId;
  nid.uCallbackMessage = WM_ICON;
  GetWindowText(currWin, nid.szTip, 128);
  if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
    return; // Don't hide window if tray icon creation failed
  }
  Shell_NotifyIcon(NIM_SETVERSION, &nid);
  context->icons[context->iconIndex].icon = nid;
  context->icons[context->iconIndex].window = currWin;
  context->iconIndex++;
  ShowWindow(currWin, SW_HIDE);
  if (!restoreWindow) {
    save(context);
  }
}

// Shows all hidden windows
void showAllWindows(TRCONTEXT *context) {
  for (int i = 0; i < context->iconIndex; i++)
  {
    ShowWindow(context->icons[i].window, SW_SHOW);
    Shell_NotifyIcon(NIM_DELETE, &context->icons[i].icon);
    context->icons[i] = {};
  }
  save(context);
  context->iconIndex = 0;
}

// Re-adds tray icons for all currently hidden windows.
// Called when Explorer restarts (TaskbarCreated) — the old icons are gone
// but the windows are still hidden, so we re-register them.
// Re-extracts icons fresh because stored HICON handles from
// GetClassLongPtr/SendMessage(WM_GETICON) go stale after Explorer restarts.
void recreateIcons(TRCONTEXT *context) {
  for (int i = 0; i < context->iconIndex; i++) {
    if (!context->icons[i].window) continue;
    HWND hwnd = context->icons[i].window;
    // Window may have been closed while we weren't looking
    if (!IsWindow(hwnd)) {
      context->icons[i] = {};
      continue;
    }
    // Re-extract icon fresh from the process executable
    HICON freshIcon = NULL;
    {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
          char exePath[MAX_PATH];
          DWORD pathLen = MAX_PATH;
          if (QueryFullProcessImageName(proc, 0, exePath, &pathLen)) {
            freshIcon = ExtractIcon(GetModuleHandle(NULL), exePath, 0);
            if (freshIcon == (HICON)1) freshIcon = NULL;
          }
          CloseHandle(proc);
        }
      }
    }
    // Fall back to window icon queries if exe extraction failed
    if (!freshIcon) {
      ULONG_PTR ic = GetClassLongPtr(hwnd, GCLP_HICONSM);
      if (!ic) ic = SendMessage(hwnd, WM_GETICON, 2, NULL);
      if (!ic) ic = SendMessage(hwnd, WM_GETICON, 0, NULL);
      if (!ic) ic = SendMessage(hwnd, WM_GETICON, 1, NULL);
      if (ic) freshIcon = (HICON)ic;
    }
    if (freshIcon) {
      context->icons[i].icon.hIcon = freshIcon;
    }
    // Update tooltip in case window title changed
    GetWindowText(hwnd, context->icons[i].icon.szTip, 128);
    Shell_NotifyIcon(NIM_DELETE, &context->icons[i].icon);
    Shell_NotifyIcon(NIM_ADD, &context->icons[i].icon);
    Shell_NotifyIcon(NIM_SETVERSION, &context->icons[i].icon);
  }
  // Compact out any dead entries
  std::vector<HIDDEN_WINDOW> temp(context->iconIndex);
  int count = 0;
  for (int i = 0; i < context->iconIndex; i++) {
    if (context->icons[i].window) {
      temp[count++] = context->icons[i];
    }
  }
  if (count != context->iconIndex) {
    memcpy_s(context->icons, sizeof(context->icons), &temp.front(), sizeof(HIDDEN_WINDOW) * context->iconIndex);
    context->iconIndex = count;
    save(context);
  }
}

void exitApp() {
  PostQuitMessage(0);
}

// Creates and reads the save file to restore hidden windows in case of unexpected termination
void startup(TRCONTEXT *context) {
  if ((saveFile = CreateFile("traymond.dat", GENERIC_READ | GENERIC_WRITE, \
    0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE) {
    MessageBox(NULL, "Error! Traymond could not create a save file.", "Traymond", MB_OK | MB_ICONERROR);
    exitApp();
    return;
  }
  // Check if we've crashed (i.e. there is a save file) during current uptime and
  // if there are windows to restore, in which case restore them and
  // display a reassuring message.
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    DWORD numbytes;
    DWORD fileSize = GetFileSize(saveFile, NULL);

    if (!fileSize) {
      return;
    };

    FILETIME saveFileWriteTime;
    GetFileTime(saveFile, NULL, NULL, &saveFileWriteTime);
    uint64_t writeTime = ((uint64_t)saveFileWriteTime.dwHighDateTime << 32 | (uint64_t)saveFileWriteTime.dwLowDateTime) / 10000;
    GetSystemTimeAsFileTime(&saveFileWriteTime);
    writeTime = (((uint64_t)saveFileWriteTime.dwHighDateTime << 32 | (uint64_t)saveFileWriteTime.dwLowDateTime) / 10000) - writeTime;

    if (GetTickCount64() < writeTime) {
      return;
    }

    std::vector<char> contents = std::vector<char>(fileSize);
    ReadFile(saveFile, &contents.front(), fileSize, &numbytes, NULL);
    char handle[20]; // x64 handles can exceed 10 digits
    int index = 0;
    for (size_t i = 0; i < fileSize; i++)
    {
      if (contents[i] != ',') {
        if (index < (int)(sizeof(handle) - 1)) { // bounds check
          handle[index] = contents[i];
          index++;
        }
      }
      else {
        handle[index] = '\0';
        index = 0;
        try {
          minimizeToTray(context, std::stoll(std::string(handle)));
        } catch (...) {} // corrupt save file entry — skip
        memset(handle, 0, sizeof(handle));
      }
    }
    std::string restore_message = "Traymond had previously been terminated unexpectedly.\n\nRestored " + \
      std::to_string(context->iconIndex) + (context->iconIndex > 1 ? " icons." : " icon.");
    MessageBox(NULL, restore_message.c_str(), "Traymond", MB_OK);
    }
  }

// Geometric minimize-button detection for windows with custom title bars
// (Electron, CEF, etc.) that return HTCLIENT for the entire window.
// DWM caption buttons are 46 DIP wide at 96 DPI. SM_CXSIZE reports the
// classic width (~36) which is too narrow, so we query DPI and compute.
static bool isInMinimizeButtonRegion(HWND hwnd, POINT pt) {
  LONG style = GetWindowLong(hwnd, GWL_STYLE);
  // Must have a minimize box and a caption bar
  if (!(style & WS_MINIMIZEBOX) || !(style & WS_CAPTION))
    return false;
  // Skip tool windows (small title bar, no min button)
  LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
  if (exStyle & WS_EX_TOOLWINDOW)
    return false;

  RECT wr;
  if (!GetWindowRect(hwnd, &wr))
    return false;

  // Get DPI for this window (falls back to 96 on older builds)
  int dpi = 96;
  HMODULE user32 = GetModuleHandle("user32.dll");
  if (user32) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    auto fn = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
    if (fn) dpi = fn(hwnd);
  }

  // DWM caption button is 46 DIP wide, caption area ~31 DIP tall
  int btnW = MulDiv(46, dpi, 96);
  int capH = MulDiv(31, dpi, 96);
  // DWM invisible border (~7 DIP on Win10/11)
  int borderW = MulDiv(7, dpi, 96);

  // Vertical check: point must be within the caption area
  if (pt.y < wr.top || pt.y > wr.top + capH)
    return false;

  // Horizontal: minimize is the 3rd button from the right when maximize
  // exists, 2nd when it doesn't. Close is always rightmost.
  bool hasMaximize = (style & WS_MAXIMIZEBOX) != 0;
  int buttonsToRight = hasMaximize ? 2 : 1; // close [+ maximize]
  int minRight = wr.right - borderW - buttonsToRight * btnW;
  int minLeft  = minRight - btnW;

  return (pt.x >= minLeft && pt.x <= minRight);
}

// Low-level mouse hook for right-click on minimize button
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_RBUTTONDOWN) {
    MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    POINT pt = ms->pt;
    HWND hwnd = WindowFromPoint(pt);
    if (hwnd) {
      // Get the top-level window (WindowFromPoint may return a child)
      HWND topLevel = GetAncestor(hwnd, GA_ROOT);
      if (topLevel) {
        LRESULT hitTest = 0;
        // Use SendMessageTimeout to avoid hanging on frozen windows
        BOOL responded = SendMessageTimeout(topLevel, WM_NCHITTEST, 0,
            MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG, 100, (PDWORD_PTR)&hitTest);

        if (responded && hitTest == HTMINBUTTON) {
          // Standard window — WM_NCHITTEST worked
          PostMessage(g_mainWindow, WM_TRAYMIN, 0, (LPARAM)topLevel);
          return 1;
        }

        // Fallback: geometric detection for custom title bars (Electron, etc.)
        // Only triggers when WM_NCHITTEST didn't return HTMINBUTTON
        if (isInMinimizeButtonRegion(topLevel, pt)) {
          PostMessage(g_mainWindow, WM_TRAYMIN, 0, (LPARAM)topLevel);
          return 1;
        }
      }
    }
  }
  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

  TRCONTEXT* context = reinterpret_cast<TRCONTEXT*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

  // Handle Explorer restart — re-add all tray icons that were destroyed.
  // WM_TASKBARCREATED is a registered message (runtime value), can't use switch.
  if (WM_TASKBARCREATED && uMsg == WM_TASKBARCREATED && context) {
    recreateIcons(context);
    return 0;
  }

  switch (uMsg)
  {
  case WM_ICON:
    // Single click restore (was WM_LBUTTONDBLCLK)
    if (LOWORD(lParam) == WM_LBUTTONUP) {
      showWindow(context, lParam);
    }
    break;
  case WM_TRAYMIN:
    // Right-click on minimize button — minimize to tray
    minimizeToTray(context, (LONG_PTR)lParam);
    break;
  case WM_HOTKEY:
    if (wParam == HOTKEY_MINIMIZE) {
      minimizeToTray(context, 0);
    } else if (wParam == HOTKEY_EXIT) {
      showAllWindows(context);
      exitApp();
    }
    break;
  default:
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
  return 0;
}

#pragma warning( push )
#pragma warning( disable : 4100 )
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
#pragma warning( pop )

  TRCONTEXT context = {};

  // Mutex to allow only one instance
  const char szUniqueNamedMutex[] = "traymond_mutex";
  HANDLE mutex = CreateMutex(NULL, TRUE, szUniqueNamedMutex);
  if (GetLastError() == ERROR_ALREADY_EXISTS)
  {
    MessageBox(NULL, "Error! Another instance of Traymond is already running.", "Traymond", MB_OK | MB_ICONERROR);
    return 1;
  }

  BOOL bRet;
  MSG msg;

  const char CLASS_NAME[] = "Traymond";

  WNDCLASS wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = CLASS_NAME;

  if (!RegisterClass(&wc)) {
    return 1;
  }

  // NOT HWND_MESSAGE â€” message-only windows are excluded from HWND_BROADCAST,
  // so they never receive TaskbarCreated. Use a regular invisible window instead.
  context.mainWindow = CreateWindow(CLASS_NAME, NULL, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

  if (!context.mainWindow) {
    return 1;
  }

  // Store our context in main window for retrieval by WindowProc
  SetWindowLongPtr(context.mainWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));

  // Set up globals for mouse hook callback
  g_mainWindow = context.mainWindow;

  // Register hotkeys: Win+Shift+Z to minimize, Win+Shift+X to restore all + exit
  if (!RegisterHotKey(context.mainWindow, HOTKEY_MINIMIZE, MOD_KEY | MOD_NOREPEAT, TRAY_KEY)) {
    MessageBox(NULL, "Error! Could not register the minimize hotkey (Win+Shift+Z).", "Traymond", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (!RegisterHotKey(context.mainWindow, HOTKEY_EXIT, MOD_KEY | MOD_NOREPEAT, EXIT_KEY)) {
    MessageBox(NULL, "Error! Could not register the exit hotkey (Win+Shift+X).", "Traymond", MB_OK | MB_ICONERROR);
    return 1;
  }

  // Install low-level mouse hook for right-click on minimize button
  g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
  if (!g_mouseHook) {
    MessageBox(NULL, "Warning: Could not install mouse hook.\nRight-click minimize will not work.", "Traymond", MB_OK | MB_ICONWARNING);
  }

  // Register for Explorer restart notification so we can re-add tray icons
  WM_TASKBARCREATED = RegisterWindowMessage("TaskbarCreated");
  // Allow TaskbarCreated through UIPI message filter (Explorer may
  // restart at a different integrity level on some Windows editions).
  ChangeWindowMessageFilter(WM_TASKBARCREATED, MSGFLT_ADD);

  // No tray icon for Traymond itself — only minimized app icons appear
  startup(&context);

  while ((bRet = GetMessage(&msg, 0, 0, 0)) != 0)
  {
    if (bRet != -1) {
      DispatchMessage(&msg);
    }
  }

  // Clean up on exit
  showAllWindows(&context);
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
  }
  UnregisterHotKey(context.mainWindow, HOTKEY_MINIMIZE);
  UnregisterHotKey(context.mainWindow, HOTKEY_EXIT);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  CloseHandle(saveFile);
  DestroyWindow(context.mainWindow);
  DeleteFile("traymond.dat"); // No save file means we have exited gracefully
  return msg.wParam;
}
