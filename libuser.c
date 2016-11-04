/*
 *  File:  libuser.c
 *
 *  Description:  This file contains the interface declarations
 *                to the OS kernel support package.
 *
 */

#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <libuser.h>
#include <usyscall.h>
#include <usloss.h>

#define CHECKMODE {    \
    if (USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE) { \
        USLOSS_Console("Trying to invoke syscall from kernel\n"); \
        USLOSS_Halt(1);  \
    }  \
}


int Sleep(int seconds)
{
    systemArgs sysArg;
    CHECKMODE;
    sysArg.number = SYS_SLEEP;
    sysArg.arg1 = seconds;

    USLOSS_Syscall(&sysArg);

    return sysArg.arg4;
}


int DiskRead(void *dbuff, int unit, int track, int first, int sectors, int *status)
{
    systemArgs sysArg;
    
    CHECKMODE;
    sysArg.number = SYS_DISKREAD;
    sysArg.arg1 = dbuff;
    sysArg.arg2 = sectors;
    sysArg.arg3 = track;
    sysArg.arg4 = first;
    sysArg.arg5 = unit;

    USLOSS_Syscall(&sysArg);

    if (sysArg.arg1 != 0 || sysArg.arg4 == -1) {
        return -1;   
    }

    return 0;
}


int DiskWrite(void *dbuff, int unit, int track, int first, int sectors, int *status)
{
    systemArgs sysArg;
    
    CHECKMODE;
    sysArg.number = SYS_DISKWRITE;
    sysArg.arg1 = dbuff;
    sysArg.arg2 = sectors;
    sysArg.arg3 = track;
    sysArg.arg4 = first;
    sysArg.arg5 = unit;

    USLOSS_Syscall(&sysArg);

    if (sysArg.arg1 != 0 || sysArg.arg4 == -1) {
        return -1;   
    }

    return 0;
}


int DiskSize(int unit, int *sector, int *track, int *disk)
{
    systemArgs sysArg;
    
    CHECKMODE;
    sysArg.number = SYS_DISKSIZE;
    sysArg.arg1 = unit;

    USLOSS_Syscall(&sysArg);

    *sector = sysArg.arg1;
    *track = sysArg.arg2;
    *disk = sysArg.arg3;

    return sysArg.arg4;
}


int TermRead(char *buff, int bsize, int unit_id, int *nread)
{
    systemArgs sysArg;
    
    CHECKMODE;
    sysArg.number = SYS_TERMREAD;
    sysArg.arg1 = buff;
    sysArg.arg2 = bsize;
    sysArg.arg3 = unit_id;

    USLOSS_Syscall(&sysArg);

    *nread = sysArg.arg2;
    return sysArg.arg4;
}


int TermWrite(char *buff, int bsize, int unit_id, int *nwrite)
{
    systemArgs sysArg;
    
    CHECKMODE;
    sysArg.number = SYS_TERMWRITE;
    sysArg.arg1 = buff;
    sysArg.arg2 = bsize;
    sysArg.arg3 = unit_id;

    USLOSS_Syscall(&sysArg);

    *nwrite = sysArg.arg2;
    return sysArg.arg4;



}


