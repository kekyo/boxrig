/* An owned foreground window for cursor-focus video checks - Under MIT. */
#include <windows.h>

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int main(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_procedure;
    window_class.lpszClassName = L"boxrig_blink_focus_peer";
    if (RegisterClassExW(&window_class) == 0) return 1;
    HWND window = CreateWindowExW(0, window_class.lpszClassName,
        L"boxrig blink focus peer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        700, 500, 400, 250, NULL, NULL, instance, NULL);
    if (window == NULL) return 1;
    MSG message;
    int status;
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return status < 0 ? 1 : 0;
}
