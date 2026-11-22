/* SPDX-License-Identifier: MIT */
/*
 * SHELL32: ShellExecuteA, which games use to open their web site or manual. Nothing is
 * started from here; the call reports success (a value above 32) so the game goes on.
 * Also what MFC programs ask for: file information, icons and dropped files (none). The
 * printer spooler (WINSPOOL.DRV) is here too: there are no printers.
 */
#include "shell32.h"

#include "rc/log.h"

#define SH "SHELL32.dll"

/* ShellExecuteA(hwnd, operation, file, parameters, directory, show) */
static void k_ShellExecuteA(rc_cpu *c)
{
    const char *op = rc_str(RC_ARG(c, 1)), *file = rc_str(RC_ARG(c, 2));
    rc_log("ShellExecuteA(%s, \"%s\"): not started", op ? op : "open", file ? file : "");
    RC_RETURN(c, 42, 6);
}

/* ShellExecuteExA(SHELLEXECUTEINFOA*): the same; hInstApp (at +32) says success, no process. */
static void k_ShellExecuteExA(rc_cpu *c)
{
    uint32_t info = RC_ARG(c, 0);
    const char *file = rc_str(rc_ld32(info + 16));
    rc_log("ShellExecuteExA(\"%s\"): not started", file ? file : "");
    rc_st32(info + 32, 42);
    if (rc_ld32(info + 4) & 0x40u) { /* SEE_MASK_NOCLOSEPROCESS: no process handle */
        rc_st32(info + 56, 0);
    }
    RC_RETURN(c, 1, 1);
}

/* SHGetFileInfoA(path, attributes, info, size, flags): nothing known. */
static void k_SHGetFileInfoA(rc_cpu *c) { RC_RETURN(c, 0, 5); }
static void k_DragAcceptFiles(rc_cpu *c) { RC_RETURN(c, 0, 2); }
/* DragQueryFileA(drop, index, buffer, size): no files are ever dropped. */
static void k_DragQueryFileA(rc_cpu *c) { RC_RETURN(c, 0, 4); }
static void k_DragFinish(rc_cpu *c) { RC_RETURN(c, 0, 1); }
/* ExtractIconA(instance, file, index) */
static void k_ExtractIconA(rc_cpu *c) { RC_RETURN(c, 0, 3); }

#define SPOOL "WINSPOOL.DRV"
/* OpenPrinterA(name, handle*, defaults) */
static void k_OpenPrinterA(rc_cpu *c)
{
    rc_last_error = 1801; /* ERROR_INVALID_PRINTER_NAME */
    RC_RETURN(c, 0, 3);
}
static void k_ClosePrinter(rc_cpu *c) { RC_RETURN(c, 1, 1); }
/* DocumentPropertiesA(hwnd, printer, name, out, in, mode) */
static void k_DocumentPropertiesA(rc_cpu *c) { RC_RETURN(c, 0xffffffffu, 6); }

const rc_import_def rc_shell32_defs[] = {
    {SH, "SHGetFileInfoA", k_SHGetFileInfoA},
    {SH, "DragAcceptFiles", k_DragAcceptFiles},
    {SH, "DragQueryFileA", k_DragQueryFileA},
    {SH, "DragFinish", k_DragFinish},
    {SH, "ExtractIconA", k_ExtractIconA},
    {SPOOL, "OpenPrinterA", k_OpenPrinterA},
    {SPOOL, "ClosePrinter", k_ClosePrinter},
    {SPOOL, "DocumentPropertiesA", k_DocumentPropertiesA},
    {SH, "ShellExecuteExA", k_ShellExecuteExA},
    {SH, "ShellExecuteA", k_ShellExecuteA},
};
const uint32_t rc_shell32_count = sizeof rc_shell32_defs / sizeof rc_shell32_defs[0];
