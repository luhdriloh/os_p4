#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <providedPrototypes.h>


#include <stdlib.h> /* needed for atoi() */
#include <stdio.h>
    
proc processTable[MAXPROC];
procPtr sleepingHead;

// /* queues for disk requests */
diskRequestPtr disks[2];
int diskMailboxes[2];

/* array of disk requests */
diskRequest diskRequests[MAXPROC];

/* number of tracks in a disk */
int disk0Tracks;
int disk1Tracks;

static int ClockDriver(char *);
static int DiskDriver(char *);


void start3(void)
{
    char	name[128];
    char    buf[MAXLINE];
    int		i;
    int		clockPID;
    int		disk0PID;
    int     disk1PID;
    int     pid;
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
        diskRequests[i].mailbox = processTable[i].mailbox;
    }


    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    
    if (clockPID < 0) {
    	USLOSS_Console("start3(): Can't create clock driver\n");
    	USLOSS_Halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "semRunning" once it is running.
     */


    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */


    diskMailboxes[0] = MboxCreate(0, 0);
    diskMailboxes[1] = MboxCreate(0, 0);

    for (i = 0; i < USLOSS_DISK_UNITS; i++) {
        sprintf(name, "Disk driver %d", i);
        sprintf(buf, "%d", i);
        pid = fork1(name, DiskDriver, buf, USLOSS_MIN_STACK, 2);

        if (pid < 0) {
            USLOSS_Console("start3(): Can't create disk driver %d\n", i);
            USLOSS_Halt(1);
        }

        if (i == 0) {
            disk0PID = pid;
        }
        else {
            disk1PID = pid;
        }

    }

    getAmountOfTracks();

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

    /* unblock disk drivers to zap them */
    MboxSend(diskMailboxes[DISK0], NULL, 0);
    MboxSend(diskMailboxes[DISK1], NULL, 0);
    zap(disk0PID);
    zap(disk1PID);

    quit(0);
}

static int ClockDriver(char *arg)
{
    int result, status, currentTime;
    procPtr toWakeUp;

    // Let the parent know we are running and enable interrupts.
    // TODO: we need to add mailboxes for mutual exclusion
    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while(! isZapped()) {
    	result = waitDevice(USLOSS_CLOCK_DEV, 0, &status);
    	if (result != 0) {
    	    return 0;
    	}

        currentTime = USLOSS_Clock();

        while (sleepingHead != NULL && sleepingHead->timeToWakeUp < currentTime) {
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
    args->arg4 = sleepReal(seconds);
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
    
    // block process
    MboxReceive(sleepProc->mailbox, NULL, 0);

    return 0;
}


static int DiskDriver(char *arg)
{
    int currentTrack, requestType, currentSector;
    int numSectors, track, unit, status;
    void *buffStart;
    diskRequestPtr request;

    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    unit = atoi((char *)arg);
    track = 0;
    currentTrack = 1;

    /* set current track number to 0 to start off with */
    status = checkTrack(&currentTrack, track, unit);
    checkDeviceStatus(status, "DiskDriver(): setting track to 0");

    /* while loop to fill disk driver requests */
    while (1) {
        MboxReceive(diskMailboxes[unit], NULL, 0);
        request = removeFromDiskQueue(unit);

        /* if we have nothing to do we return */
        if (request == NULL) {
            return 0;
        }

        /* initialize information taken from the current disk request */
        track = request->track;
        currentSector = request->startSector;
        numSectors = request->numSectors;
        requestType = request->type;


        /* loop through the number of sectors we need to read */
        for (int i = 0; i < numSectors; i++) {
            
            /* Check if we need to continue on to the next track */
            if (currentSector > 15) {
                track++;
                currentSector = 0;
            }

            /* Check if we are in the correct track. If error occurs break */
            status = checkTrack(&currentTrack, track, unit);
            if (status == USLOSS_DEV_ERROR) {
                break;
            }

            /* offset the buffer to the correct position */
            buffStart = (char *)request->buffer + (512 * i);

            /* send disk request */
            status = runDiskRequest(unit, requestType, currentSector, buffStart);
            if (status == USLOSS_DEV_ERROR) {
                break;
            }

            currentSector++;
        }

        /* release the mailbox with the status returned */
        request->result = status;
        MboxSend(request->mailbox, NULL, 0);
    }

    return 0;
}


void diskRead(systemArgs *args)
{
    void *buf;
    int unit, track, first, sectors;

    buf = args->arg1;
    unit = args->arg5;
    track = args->arg3;
    first = args->arg5;
    sectors = args->arg2;

    int result = diskReadReal(buf, unit, track, first, sectors);

    if (result == -1) {
        args->arg1 = -1;
        args->arg4 = -1;
        return;
    }

    args->arg1 = result;
    args->arg4 = 0;
}


int diskReadReal(void *dbuff, int unit, int track, int first, int sectors)
{
    if (verifyDiskParameters(dbuff, unit, first)) {
        return -1;
    }

    int procIndex =  getpid() % MAXPROC;

    diskRequestPtr request = &diskRequests[procIndex];
    request->type = USLOSS_DISK_READ;
    request->buffer = dbuff;
    request->track = track;
    request->startSector = first;
    request->numSectors = sectors;

    addToDiskQueue(unit, request);

    // wake up disk driver 
    MboxSend(diskMailboxes[unit], NULL, 0);

    // block process
    MboxReceive(request->mailbox, NULL, 0);
    return request->result;
}



void diskWrite(systemArgs *args)
{
    void *buf;
    int unit, track, first, sectors;

    buf = args->arg1;
    unit = args->arg5;
    track = args->arg3;
    first = args->arg5;
    sectors = args->arg2;

    int result = diskWriteReal(buf, unit, track, first, sectors);

    if (result == -1) {
        args->arg1 = -1;
        args->arg4 = -1;
        return;
    }

    args->arg1 = result;
    args->arg4 = 0;
      
}


int diskWriteReal(void *dbuff, int unit, int track, int first, int sectors)
{
    if (verifyDiskParameters(dbuff, unit, first)) {
        return -1;
    }

    int procIndex =  getpid() % MAXPROC;

    /* create and fill our request struct */
    diskRequestPtr request = &diskRequests[procIndex];

    request->type = USLOSS_DISK_READ;
    request->buffer = dbuff;
    request->track = track;
    request->startSector = first;
    request->numSectors = sectors;

    addToDiskQueue(unit, request);

    // wake up disk driver 
    MboxSend(diskMailboxes[unit], NULL, 0);

    // block process
    MboxReceive(request->mailbox, NULL, 0);
    return request->result;

}

int verifyDiskParameters(void *dbuff, int unit, int first)
{
    if (unit != 0 && unit != 1) {
        return 1;
    }

    if (first < 0 || first > 15) {
        return 1;
    }

    if (dbuff == NULL) {
        return 1;
    }

    return 0;
}


void diskSize(systemArgs *args)
{
    int sector, track, disk, unit;

    unit = args->arg1;
    args->arg4 = diskSizeReal(unit, &sector, &track, &disk);
}


int diskSizeReal(int unit, int *sector, int *track, int *disk)
{
    int tracks;
    USLOSS_DeviceRequest deviceRequest;

    /* check if we have a correct unit */
    if (unit != DISK0 && unit != DISK1) {
        return -1;
    }
   
    *sector = USLOSS_DISK_SECTOR_SIZE;
    *track = USLOSS_DISK_TRACK_SIZE;
    *disk = unit ? disk1Tracks : disk0Tracks;

    return 0;
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


void addToDiskQueue(int unit, diskRequestPtr request)
{
    diskRequestPtr walk = disks[unit];
    request->next = NULL;

    if (walk == NULL) {
        disks[unit] = request;
    }

    while (walk->next != NULL &&
           !(walk->track < request->track && walk->next->track > request->track)) {
        walk = walk->next;
    }

    request->next = walk->next;
    walk->next = request;
}


diskRequestPtr removeFromDiskQueue(int unit)
{
    diskRequestPtr temp = disks[unit];

    if (temp == NULL) {
        return NULL;
    }

    disks[unit] = temp->next;
    return temp;
}


void fillDeviceRequest(USLOSS_DeviceRequest *request, int opr, void *reg1, void *reg2)
{
    request->opr = opr;
    request->reg1 = reg1;
    request->reg2 = reg2;
}


void getAmountOfTracks()
{
    int status;

    /* send disk request for amount of tracks for disk0 */
    status = runDiskRequest(DISK0, USLOSS_DISK_TRACKS, &disk0Tracks, NULL);
    checkDeviceStatus(status, "getAmountOfTracks(): disk0 ");


    /* send disk request for amount of tracks for disk1 */
    status = runDiskRequest(DISK1, USLOSS_DISK_TRACKS, &disk1Tracks, NULL);
    checkDeviceStatus(status, "getAmountOfTracks(): disk1 ");
}


int checkTrack(int *currentTrack, int track, int diskNumber)
{
    int status;

    status = 0;

    /* check if we are in the correct track */
    if (*currentTrack != track) {
        /* move arm to correct track */
        status = runDiskRequest(diskNumber, USLOSS_DISK_SEEK, track, NULL);

        /* set the current track to the new position */
        if (status == USLOSS_DEV_READY) {
            *currentTrack = track;
        }
    }

    return status;
}


int runDiskRequest(int diskNumber, int operation, void *reg1, void *reg2)
{
    return runRequest(USLOSS_DISK_DEV, diskNumber, operation, reg1, reg2);
}


void checkDeviceStatus(int status, char *name)
{
    if (status) {
        USLOSS_Console("status: %d\n", status);
        USLOSS_Console("%s: error in finding size of disk. Halting...", name);
        USLOSS_Halt(1);
    }
}


int runRequest(int typeDevice, int deviceNum, int operation, void *reg1, void *reg2)
{
    int status;
    USLOSS_DeviceRequest deviceRequest;

    /* send disk request for amount of tracks for disk0 */
    fillDeviceRequest(&deviceRequest, operation, reg1, reg2);
    status = USLOSS_DeviceOutput(typeDevice, deviceNum, &deviceRequest);
    waitDevice(typeDevice, deviceNum + 1, &status);

    return status;
}


















