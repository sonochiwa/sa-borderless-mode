#include "input/message_pump.h"

#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"
#include "input/key_filter.h"

namespace bm {
namespace {

using PeekMessageTFn = BOOL (WINAPI*)(LPMSG msg, HWND window, UINT filterMin,
                                      UINT filterMax, UINT removeFlags);
using GetMessageTFn = BOOL (WINAPI*)(LPMSG msg, HWND window, UINT filterMin,
                                     UINT filterMax);

PeekMessageTFn g_originalPeekMessageA = nullptr;
PeekMessageTFn g_originalPeekMessageW = nullptr;
GetMessageTFn g_originalGetMessageA = nullptr;
GetMessageTFn g_originalGetMessageW = nullptr;
HHOOK g_getMessageHook = nullptr;

void FilterQueuedInputMessage(MSG* msg) {
    __try {
        if (!msg) {
            return;
        }
        // MoonLoader's consumeWindowMessage(true, true) neutralizes this
        // event while the game reads its queue, before SA-MP's window-mode
        // handler can act on it.
        if (msg->message == WM_SYSKEYUP && msg->wParam == VK_RETURN) {
            Log("pump WM_SYSKEYUP/VK_RETURN neutralized");
            msg->message = WM_NULL;
            return;
        }
        if (msg->wParam != VK_TAB) {
            return;
        }
        bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
        bool up = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
        if (!down && !up) {
            return;
        }
        bool altOrWin = AltOrWinHeldAsync();
        if (altOrWin || TabRefocusGraceActive()) {
            Log("pump TAB %s neutralized (%s)", down ? "keydown" : "keyup",
                altOrWin ? "alt/win" : "grace");
            msg->message = WM_NULL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

BOOL WINAPI HookedPeekMessageA(LPMSG msg, HWND window, UINT filterMin,
                               UINT filterMax, UINT removeFlags) {
    BOOL result = g_originalPeekMessageA(msg, window, filterMin, filterMax,
                                         removeFlags);
    if (result) {
        FilterQueuedInputMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedPeekMessageW(LPMSG msg, HWND window, UINT filterMin,
                               UINT filterMax, UINT removeFlags) {
    BOOL result = g_originalPeekMessageW(msg, window, filterMin, filterMax,
                                         removeFlags);
    if (result) {
        FilterQueuedInputMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedGetMessageA(LPMSG msg, HWND window, UINT filterMin,
                              UINT filterMax) {
    BOOL result = g_originalGetMessageA(msg, window, filterMin, filterMax);
    if (result != 0 && result != -1) {
        FilterQueuedInputMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedGetMessageW(LPMSG msg, HWND window, UINT filterMin,
                              UINT filterMax) {
    BOOL result = g_originalGetMessageW(msg, window, filterMin, filterMax);
    if (result != 0 && result != -1) {
        FilterQueuedInputMessage(msg);
    }
    return result;
}

LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        FilterQueuedInputMessage(reinterpret_cast<MSG*>(lParam));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

void HookMessagePump() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    InstallExportHook(user32, "PeekMessageA",
                      reinterpret_cast<void*>(&HookedPeekMessageA),
                      reinterpret_cast<void**>(&g_originalPeekMessageA));
    InstallExportHook(user32, "PeekMessageW",
                      reinterpret_cast<void*>(&HookedPeekMessageW),
                      reinterpret_cast<void**>(&g_originalPeekMessageW));
    InstallExportHook(user32, "GetMessageA",
                      reinterpret_cast<void*>(&HookedGetMessageA),
                      reinterpret_cast<void**>(&g_originalGetMessageA));
    InstallExportHook(user32, "GetMessageW",
                      reinterpret_cast<void*>(&HookedGetMessageW),
                      reinterpret_cast<void**>(&g_originalGetMessageW));
}

void InstallGetMessageHook(HWND window) {
    if (g_getMessageHook || !window || !IsWindow(window)) {
        return;
    }
    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    g_getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, &GetMsgHookProc,
                                         SelfModule(), threadId);
    Log("WH_GETMESSAGE hook: hook=0x%p thread=%lu error=%lu",
        g_getMessageHook, threadId,
        g_getMessageHook ? 0 : GetLastError());
}

void RemoveGetMessageHook() {
    if (g_getMessageHook) {
        UnhookWindowsHookEx(g_getMessageHook);
        g_getMessageHook = nullptr;
    }
}

}  // namespace bm
