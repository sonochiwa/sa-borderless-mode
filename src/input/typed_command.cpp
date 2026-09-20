#include "input/typed_command.h"

#include <cstring>

namespace bm {
namespace {

constexpr size_t kWordCapacity = 32;

// The same idea as CCheat::DoCheats: the last keys typed are kept in order
// and the word matches as soon as it is the tail of that history.
SRWLOCK g_lock = SRWLOCK_INIT;
char g_word[kWordCapacity] = {};
size_t g_wordLength = 0;
char g_history[kWordCapacity] = {};
size_t g_historyLength = 0;
volatile LONG g_pending = 0;

// The same message can be seen twice when one PeekMessage export forwards to
// another; a key-down carries the queue time of the press, so a repeat of the
// same key at the same time is the same press.
DWORD g_lastKeyTime = 0;
WPARAM g_lastKey = 0;

char KeyToChar(WPARAM virtualKey) {
    if ((virtualKey >= 'A' && virtualKey <= 'Z') ||
        (virtualKey >= '0' && virtualKey <= '9')) {
        return static_cast<char>(virtualKey);
    }
    if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9) {
        return static_cast<char>('0' + (virtualKey - VK_NUMPAD0));
    }
    return 0;
}

}  // namespace

void SetTypedCommandWord(const char* word) {
    char normalized[kWordCapacity] = {};
    size_t length = 0;
    for (const char* c = word ? word : ""; *c && length < kWordCapacity - 1; ++c) {
        if (*c >= 'a' && *c <= 'z') {
            normalized[length++] = static_cast<char>(*c - 'a' + 'A');
        } else if ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) {
            normalized[length++] = *c;
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_word, normalized, kWordCapacity);
    g_wordLength = length;
    g_historyLength = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void RecordTypedKey(const MSG* msg) {
    if (!msg || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) {
        return;
    }
    // Bit 30 of a key-down lParam is the previous key state: set on
    // auto-repeat. Only fresh presses count.
    if (msg->lParam & (1L << 30)) {
        return;
    }
    const char typed = KeyToChar(msg->wParam);
    if (!typed) {
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    if (msg->time == g_lastKeyTime && msg->wParam == g_lastKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_lastKeyTime = msg->time;
    g_lastKey = msg->wParam;
    if (g_historyLength == kWordCapacity - 1) {
        std::memmove(g_history, g_history + 1, kWordCapacity - 2);
        --g_historyLength;
    }
    g_history[g_historyLength++] = typed;
    if (g_wordLength != 0 && g_historyLength >= g_wordLength &&
        std::memcmp(g_history + g_historyLength - g_wordLength, g_word,
                    g_wordLength) == 0) {
        g_historyLength = 0;
        InterlockedExchange(&g_pending, 1);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool ConsumeTypedCommand() {
    return InterlockedExchange(&g_pending, 0) != 0;
}

}  // namespace bm
