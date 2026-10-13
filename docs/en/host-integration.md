# Handling terminal requests to the host

[日本語](../ja/host-integration.md)

Control sequences from programs running in the terminal can request window title changes, clipboard access, and desktop notifications.
boxrig core parses the requests; the application embedding the terminal performs the OS operations.
As with rendering and input, the application chooses which operations to allow and how to connect them to the OS.

## Receive title and other requests

Read the first request with `boxrig_terminal_host_item_peek()`.
It provides the kind, flags, length-delimited payload, and a request ID when needed.
Borrowed data remains valid until the next terminal mutation; copy what you need if retaining it asynchronously.

This function handles one item in an application that does not provide OS integration.
Pass a terminal after feeding input; it removes the request from the queue and reports unavailable if a completion response is needed.
It also works with core alone, without a window.

```c
#include <boxrig/terminal.h>

boxrig_status discard_host_item(boxrig_terminal *terminal)
{
    boxrig_host_item item;
    if (!boxrig_terminal_host_item_peek(terminal, &item))
        return BOXRIG_STATUS_OK;

    /* Copy the ID before consuming the borrowed queue item. */
    uint64_t request_id = item.request_id;
    boxrig_status status = boxrig_terminal_host_item_consume(terminal);
    if (status != BOXRIG_STATUS_OK || request_id == 0) return status;
    return boxrig_terminal_complete_host_request(terminal, request_id,
        BOXRIG_HOST_REQUEST_UNAVAILABLE, NULL, 0);
}
```

Items that need no completion response, such as titles and bells, have request ID 0.
Title requests include window/icon targets and save/restore flags.

## Return responses and completion

For requests requiring completion, consume the request and then return a result exactly once for the same ID.
Results distinguish success, application-policy rejection, and environment unavailability.

Read requests may hold up subsequent output to preserve response ordering.
Complete requests even if you do not execute them.
You cannot complete an ID still in the queue, an unknown ID, or an already completed ID.
Successful read data is bounded by the capacity specified in the request.

## Handle the clipboard

OSC 52 supplies read and write requests.
Treat configuring accepted capacity separately from accessing the OS clipboard.
The application grants access for its intended use and returns read data or a rejection.

Normal UI paste operations can also be asynchronous.
During shutdown, stop processing so that completion notifications cannot reference destroyed terminals or windows.
Follow each platform's contract when stopping and destroying standard components.

## Handle links and working directories

An OSC 7 URI is the working directory reported by the peer.
An OSC 8 link is information attached to cells.
Receiving either one does not itself change the host directory or launch a browser.

Connect link opening to an application-selected action, such as a user click.
The application also decides which URI kinds are allowed and how to launch them.
See [boxrig_term](./boxrig_term.md) for the bundled terminal's controls.

## Connect desktop notifications

OSC 99 notifications include a title, body, identifier, urgency, and display conditions such as focus.
Connect core's normalized requests to the common [notification dispatch API](../../include/boxrig/ui/notification_dispatch.h) and a platform provider.

The Linux notification provider is an optional module using GLib/GIO.
Install it with `make install-notification PREFIX=...` and link `boxrig-notification`.
Using a GTK widget does not make a notification provider mandatory.
Handle the result when no notification service is available as well.

## Allow external image data

Kitty image file and shared-memory transfers do not let core directly open arbitrary files.
A `GRAPHICS_DATA` request includes a name, offset, size, capacity, and any request to delete the data after reading.
The host decides whether reading and deletion are permitted, then returns the result.

The caller remains responsible for permission policy even when using the [POSIX helper API](../../include/boxrig/posix.h).
Distinguish successful reading, permission to delete, and completion returned to the terminal.

## Decide application policy

Permit operations that affect the user's environment individually according to the application's purpose.
For example, a log viewer can apply titles while marking clipboard reads and external image data unavailable.

Core capacity limits do not replace host permissions.
The bundled terminal's defaults are not a policy imposed by the entire library.
Implement pending requests, unsent responses, and shutdown ordering together with [communication sessions](./sessions.md).

[Host request API](../../include/boxrig/terminal.h), [Documentation index](./index.md)
