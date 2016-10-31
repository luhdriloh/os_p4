#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <providedPrototypes.h>


#include <stdlib.h> /* needed for atoi() */
#include <stdio.h>

int semRunning;
proc processTable[MAXPROC];
procPtr sleepingHead;

static int ClockDriver(char *);
static int DiskDriver(char *);

void addToSleepingQueue(procPtr toAdd);
procPtr removeFromSleepingQueue();

void start3(void)
{
    char	name[128];
    char    termbuf[10];
    char    buf[MAXLINE];
    int		i;
    int		clockPID;
    int		pid;
    int		status;
    /*
     * Check kernel mode here.
     */



    /* initialize systemcall vector */
    // TODO: not sure if we need nullsys

    systemCallVec[SYS_SLEEP] = sleep;
    systemCallVec[SYS_DISKREAD] = diskRead;
    systemCallVec[SYS_DISKWRITE] = diskWrite;
    systemCallVec[SYS_DISKSIZE] = diskSize;
    systemCallVec[SYS_TERMREAD] = termRead;
    systemCallVec[SYS_TERMWRITE] = termWrite;

    /* set up mailboxes for each process */
    for (int i = 0; i < MAXPROC; i++) {
        processTable[i].mailbox = MboxCreate(0, 0);
    }


    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    semRunning = semcreateReal(0);
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (clockPID < 0) {
	USLOSS_Console("start3(): Can't create clock driver\n");
	USLOSS_Halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "semRunning" once it is running.
     */

    sempReal(semRunning);

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */

    for (i = 0; i < USLOSS_DISK_UNITS; i++) {
        sprintf(buf, "%d", i);
        pid = fork1(name, DiskDriver, buf, USLOSS_MIN_STACK, 2);
        if (pid < 0) {
            USLOSS_Console("start3(): Can't create term driver %d\n", i);
            USLOSS_Halt(1);
        }
    }
    sempReal(semRunning);
    sempReal(semRunning);

    /*
     * Create terminal device drivers.
     */


    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case first letters.
     */
    pid = spawnReal("start4", start4, NULL, 4 * USLOSS_MIN_STACK, 3);
    pid = waitReal(&status);

    /*
     * Zap the device drivers
     */
    zap(clockPID);  // clock driver

    // eventually, at the end:
    quit(0);
    
}

static int ClockDriver(char *arg)
{
    int result, status, currentTime;
    procPtr toWakeUp;

    // Let the parent know we are running and enable interrupts.
    semvReal(semRunning);
    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while(! isZapped()) {
    	result = waitDevice(USLOSS_CLOCK_DEV, 0, &status);
    	if (result != 0) {
    	    return 0;
    	}

        currentTime = USLOSS_Clock();

        while (sleepingHead != NULL && sleepingHead->timeToWakeUp > currentTime) {
            toWakeUp = removeFromSleepingQueue();
            if (toWakeUp != NULL) {
                toWakeUp->status = NOT_USED;
                MboxCondSend(toWakeUp->mailbox, NULL, 0);
            }

        }
    }
}


void sleep(systemArgs *args)
{
    int seconds;

    seconds = args->arg1;
    args->arg4 = sleapReal(seconds);
}


int sleepReal(int seconds)
{
    if (seconds < 0) {
        return -1;
    }

    int pid, sleepProcTableIndex, timeToWakeUp;
    procPtr sleepProc;

    // TODO: create proc and put into que
    pid = getpid();
    sleepProcTableIndex = pid % MAXPROC;
    timeToWakeUp = USLOSS_Clock() + (seconds * 1000000);
    sleepProc = &processTable[sleepProcTableIndex];

    if (sleepProc->status != NOT_USED) {
        return -1;
    }

    sleepProc->pid = pid;
    sleepProc->timeToWakeUp = timeToWakeUp;
    sleepProc->next = NULL;
    sleepProc->status = IN_USE;

    addToSleepingQueue(sleepProc);
    MboxReceive(sleepProc->mailbox, NULL, 0);

    return 0;
}


static int DiskDriver(char *arg)
{
    int unit = atoi((char *)arg);   // Unit is passed as arg.
    return 0;
}


void diskRead(systemArgs *args)
{

}


int diskReadReal(void *dbuff, int unit, int track, int first, int sectors,int *status)
{

}



void diskWrite(systemArgs *args)
{

}


int diskWriteReal(void *dbuff, int unit, int track, int first, int sectors,int *status)
{

}



void diskSize(systemArgs *args)
{

}


int diskSizeReal(int unit, int *sector, int *track, int *disk)
{

}



void termRead(systemArgs *args)
{

}


int termReadReal(char *buff, int bsize, int unit_id, int *nread)
{

}



void termWrite(systemArgs *args)
{

}


int termWriteReal(char *buff, int bsize, int unit_id, int *nwrite)
{

}





/* ------------------------------------------------------------------------
   Name - addToList
   Purpose - 
   Parameters -
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

void addToSleepingQueue(procPtr toAdd)
{  
    if (sleepingHead == NULL) {
        sleepingHead = toAdd;
        return;
    }
    
    procPtr temp = sleepingHead;

    while (temp->next != NULL && temp->next->timeToWakeUp < toAdd->timeToWakeUp ) {
        temp = temp-> next;
    }

    toAdd->next = temp->next;
    temp->next = toAdd;
}



/* ------------------------------------------------------------------------
   Name - removeFromList
   Purpose - 
   Parameters -
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

procPtr removeFromSleepingQueue()
{
    if (sleepingHead == NULL) {
        return NULL;
    }

    procPtr toRemove = sleepingHead;
    sleepingHead = sleepingHead->next;
    return toRemove;   
} 



