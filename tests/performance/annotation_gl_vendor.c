/* boxrig - Under MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) return 1;
    int attributes[] = {GLX_RGBA, None};
    XVisualInfo *visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    if (visual == NULL) { XCloseDisplay(display); return 1; }
    Colormap colormap = XCreateColormap(display, RootWindow(display, visual->screen),
        visual->visual, AllocNone);
    XSetWindowAttributes window_attributes = {.colormap = colormap};
    Window window = XCreateWindow(display, RootWindow(display, visual->screen),
        0, 0, 1, 1, 0, visual->depth, InputOutput, visual->visual,
        CWColormap, &window_attributes);
    GLXContext context = glXCreateContext(display, visual, NULL, True);
    int success = 1;
    if (context != NULL && window != None && glXMakeCurrent(display, window, context)) {
        const GLubyte *vendor = glGetString(GL_VENDOR);
        const GLubyte *renderer = glGetString(GL_RENDERER);
        const GLubyte *version = glGetString(GL_VERSION);
        if (vendor != NULL && renderer != NULL && version != NULL) {
            printf("gl_vendor=%s\ngl_renderer=%s\ngl_version=%s\n",
                (const char *)vendor, (const char *)renderer, (const char *)version);
            success = 0;
        }
        glXMakeCurrent(display, None, NULL);
    }
    if (context != NULL) glXDestroyContext(display, context);
    if (window != None) XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XFree(visual);
    XCloseDisplay(display);
    return success;
}
