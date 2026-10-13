/* boxrig - Under MIT. Owned backdrop and PTY content for physical blur checks. */
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned int frame;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_TIMER) {
        frame++;
        InvalidateRect(window, NULL, FALSE);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT bounds;
        GetClientRect(window, &bounds);
        for (LONG y = 0; y < bounds.bottom; y += 96) {
            for (LONG x = 0; x < bounds.right; x += 8) {
                bool bright = (((unsigned int)x / 8 + frame / 5) & 1) != 0;
                COLORREF color = bright ? RGB(240, 240, 240) : RGB(24, 24, 24);
                if (((unsigned int)y / 96 + frame / 25) & 1)
                    color = bright ? RGB(224, 96, 32) : RGB(32, 96, 224);
                /* Global green changes survive blur across adjacent bands. */
                color = (color & 0xff00ffu) | ((frame / 25 & 1) ? 0xd000u : 0x5000u);
                HBRUSH brush = CreateSolidBrush(color);
                RECT tile = {x, y, x + 8, y + 96};
                FillRect(dc, &tile, brush);
                DeleteObject(brush);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--terminal") == 0) {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (!GetConsoleMode(output, &mode) ||
            !SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return 1;
        if (!GetConsoleMode(input, &mode) ||
            !SetConsoleMode(input, (mode | ENABLE_VIRTUAL_TERMINAL_INPUT) &
                ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT))) return 1;
        /* Opaque Kitty pixels and text remain foreground content while the
         * animation belongs entirely to the separate backdrop window. */
        fputs("\033[2J\033[H\033[38;2;255;255;255mBOXRIG foreground text\r\n"
            "Acrylic affects the desktop behind this window.\r\n"
            "\033_Gf=32,s=1,v=1,a=T,c=12,r=4;AP8A/w==\033\\"
            "\033[8;1H\033[1 q\033]0;plan27-background-ready\007", stdout);
        fflush(stdout);
        if (!SetConsoleTitleW(L"plan27-background-ready")) return 1;
        char bytes[32]; DWORD count;
        while (ReadFile(input, bytes, sizeof(bytes), &count, NULL) && count != 0) {
            if (memchr(bytes, 'q', count) != NULL) break;
        }
        return 0;
    }
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW cls = {0};
    cls.cbSize = sizeof(cls); cls.hInstance = instance;
    cls.lpfnWndProc = procedure; cls.lpszClassName = L"boxrig_background_effect_scene";
    if (RegisterClassExW(&cls) == 0) return 1;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls.lpszClassName, L"boxrig background effect scene", WS_POPUP | WS_VISIBLE,
        20, 20, 1100, 800, NULL, NULL, instance, NULL);
    if (window == NULL) return 1;
    if (SetTimer(window, 1, 40, NULL) == 0) return 1;
    MSG message;
    int status;
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return status < 0 ? 1 : 0;
}
