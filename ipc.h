#ifndef IPC_H
#define IPC_H

/* Protocol between lumosctl.exe and the running Lumos.

   lumosctl sends one IpcRequest to the hidden main window (class
   LUMOS_MAIN_CLASS) as WM_COPYDATA, with wParam set to its own reply window.

   Lumos checks the request and answers the WM_COPYDATA at once with
   IPC_RESULT_ACCEPTED (through ReplyMessage), and only then runs the command.
   It has to: while a SendMessage from another process is being handled, COM
   refuses outgoing calls (RPC_E_CANTCALLOUT_ININPUTSYNCCALL), and the WMI
   backend for laptop panels is an out-of-process COM call. When the command is
   done, Lumos sends an IpcReply (result code and text) to the reply window as
   WM_COPYDATA, and lumosctl waits for it.

   A request rejected before it runs is answered synchronously instead: the
   IpcReply arrives during the SendMessage and the message returns
   IPC_RESULT_FAILED. A Lumos build without this protocol leaves WM_COPYDATA to
   DefWindowProc, which returns 0, so 0 is never a valid result code. */

#include <windows.h>
#include "cliparse.h"

#define LUMOS_MAIN_CLASS    L"LumosMain"

#define IPC_REQUEST_MAGIC   0x4C4D5132u   /* "LMQ2", bump the digit on any protocol change */
#define IPC_REPLY_MAGIC     0x4C4D5232u   /* "LMR2" */

#define IPC_RESULT_OK       1
#define IPC_RESULT_FAILED   2             /* the reply text says why */
#define IPC_RESULT_ACCEPTED 3             /* checked and running; an IpcReply follows */

#define IPC_REPLY_MAX       4096          /* characters, including the terminator */

typedef struct {
    UINT32 size;                    /* sizeof(IpcRequest), checked by the receiver */
    INT32  command;                 /* CLI_* from cliparse.h */
    INT32  value;
    WCHAR  name[CLI_NAME_MAX];      /* preset name */
    WCHAR  monitor[CLI_NAME_MAX];   /* number from --list or a name; empty = all */
} IpcRequest;

typedef struct {
    UINT32 size;                    /* sizeof(IpcReply) */
    INT32  result;                  /* IPC_RESULT_OK or IPC_RESULT_FAILED */
    WCHAR  text[IPC_REPLY_MAX];
} IpcReply;

#endif /* IPC_H */
