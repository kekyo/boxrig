/* Read the actual shared native cursor without changing its display count. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    CURSORINFO info = {0};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return 1;
    printf("{\"visible\":%s,\"hand\":%s,\"text\":%s,\"mousePresent\":%d}\n",
        (info.flags & CURSOR_SHOWING) != 0 ? "true" : "false",
        info.hCursor == LoadCursorW(NULL, IDC_HAND) ? "true" : "false",
        info.hCursor == LoadCursorW(NULL, IDC_IBEAM) ? "true" : "false",
        GetSystemMetrics(SM_MOUSEPRESENT));
    return 0;
}
