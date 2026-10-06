#ifndef IPC_H
#define IPC_H

/* Protocol between lumosctl.exe and the running Lumos.

   lumosctl sends one IpcRequest to the hidden main window (class
   LUMOS_MAIN_CLASS) as WM_COPYDATA, with wParam set to its own reply window.
   While handling it, Lumos sends the reply text back to that window as
   WM_COPYDATA, then returns an IPC_RESULT_* code from the original message.
   A Lumos build without this protocol leaves WM_COPYDATA to DefWindowProc,
   which returns 0, so 0 is never a valid result code. */

#include <windows.h>
#include "cliparse.h"

#define LUMOS_MAIN_CLASS    L"LumosMain"

#define IPC_REQUEST_MAGIC   0x4C4D5131u   /* "LMQ1", bump the digit on any layout change */
#define IPC_REPLY_MAGIC     0x4C4D5231u   /* "LMR1" */

#define IPC_RESULT_OK       1
#define IPC_RESULT_FAILED   2             /* the reply text says why */

#define IPC_REPLY_MAX       4096          /* characters, including the terminator */

typedef struct {
    UINT32 size;                    /* sizeof(IpcRequest), checked by the receiver */
    INT32  command;                 /* CLI_* from cliparse.h */
    INT32  value;
    WCHAR  name[CLI_NAME_MAX];      /* preset name */
    WCHAR  monitor[CLI_NAME_MAX];   /* number from --list or a name; empty = all */
} IpcRequest;

#endif /* IPC_H */
