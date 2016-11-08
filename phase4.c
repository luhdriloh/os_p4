#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <providedPrototypes.h>


#include <stdlib.h> /* needed for atoi() */
#include <stdio.h>
#include <string.h>
    
/* process table for sleeping */
proc processTable[MAXPROC];
procPtr sleepingHead;

/* stuff for disk requests */
diskRequestPtr disksRequestQueue[USLOSS_DISK_UNITS];
int diskMailboxes[USLOSS_DISK_UNITS];
int diskCurrentTrack[USLOSS_DISK_UNITS];
int diskQueMutex[USLOSS_DISK_UNITS];

/* array of disk requests */
diskRequest diskRequests[MAXPROC];

/* number of tracks in a disk */
int disk0Tracks;
int disk1Tracks;

/* for terminal read requests */
int termInMailboxes[USLOSS_TERM_UNITS];
int termLineOutMailboxes[USLOSS_TERM_UNITS];

/* for terminal write requests */
termRequest termWriteRequestsTable[MAXPROC];
termRequestPtr termWriteRequests[USLOSS_TERM_UNITS];
int terminalRequestQueMutex[USLOSS_TERM_UNITS];
int terminalWriteMailbox[USLOSS_TERM_UNITS];
int terminalWriteCharMailbox[USLOSS_TERM_UNITS];

static int ClockDriver(char *);
static int DiskDriver(char *);
static int TerminalDriver(char *);
static int TermReader(char *);
static int TermWriter(char *);

void start3(void)
{
    char	name[128];
    char    buf[MAXLINE];
    int		clockPID;
    int		diskPID[USLOSS_DISK_UNITS];


    int     termDriverPID[USLOSS_TERM_UNITS];
    int     pid;
    int		status;


    /* initialize systemcall vector */

    systemCallVec[SYS_SLEEP] = sleep;
    systemCallVec[SYS_DISKREAD] = diskRead;
    systemCallVec[SYS_DISKWRITE] = diskWrite;
    systemCallVec[SYS_DISKSIZE] = diskSize;
    systemCallVec[SYS_TERMREAD] = termRead;
    systemCallVec[SYS_TERMWRITE] = termWrite;

    /* set up mailboxes for each process */
    for (int process = 0; process < MAXPROC; process++) {
        int mailbox = MboxCreate(0, 0);

        processTable[process].mailbox = mailbox;
        diskRequests[process].mailbox = mailbox;
        termWriteRequestsTable[process].mailbox = mailbox;
    }

    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    checkForkReturnValue(clockPID, 0, "start3(): Can't create clock driver unit ");

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */

    for (int unit = 0; unit < USLOSS_DISK_UNITS; unit++) {
        /* create the mailboxes associated with each disk */
        diskMailboxes[unit] = MboxCreate(MAXPROC, 0);
        diskQueMutex[unit] = MboxCreate(1, 0);

        sprintf(name, "Disk driver %d", unit);
        sprintf(buf, "%d", unit);

        pid = fork1(name, DiskDriver, buf, USLOSS_MIN_STACK * 2, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create disk driver unit ");

        /* set pid in diskPID indexed by unit number */
        diskPID[unit] = pid;
    }

    /* set the number of tracks each of our disk has for use in diskSize */
    getAmountOfTracks();

    /*
     * Create terminal device drivers.
     */

    for (int unit = 0; unit < USLOSS_TERM_UNITS; unit++) {
        /* create mailboxes associated with each terminal */
        termInMailboxes[unit] = MboxCreate(0, 1);
        termLineOutMailboxes[unit] = MboxCreate(LINE_BUFFER_SIZE, MAXLINE);
        terminalWriteCharMailbox[unit] = MboxCreate(0, 1);
        terminalRequestQueMutex[unit] = MboxCreate(1, 0);
        terminalWriteMailbox[unit] = MboxCreate(MAXPROC, 0);

        sprintf(buf, "%d", unit);

        sprintf(name, "Terminal driver %d", unit);
        pid = fork1(name, TerminalDriver, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal driver unit ");
        termDriverPID[unit] = pid;

        sprintf(name, "Terminal reader %d", unit);
        pid = fork1(name, TermReader, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal reader unit ");

        sprintf(name, "Terminal writer %d", unit);
        pid = fork1(name, TermWriter, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal writer unit ");
    }


    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case first letters.
     */
    pid = spawnReal("start4", start4, NULL, 4 * USLOSS_MIN_STACK, 3);
    pid = waitReal(&status);
    

    /* zap clock driver */
    zap(clockPID);

    /* unblock disk drivers to zap them */
    for (int unit = 0; unit < USLOSS_DISK_UNITS; unit++) {
        MboxRelease(diskMailboxes[unit]);
    }

    /* unblock terminal drivers and zap them */
    for (int unit = 0; unit < USLOSS_TERM_UNITS; unit++) {
        MboxRelease(termInMailboxes[unit]);
        MboxRelease(terminalWriteMailbox[unit]);

        USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, &status);
        zap(termDriverPID[unit]);
    }

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
    while(!isZapped()) {
    	result = waitDevice(USLOSS_CLOCK_DEV, 0, &status);
    	if (result != 0) {
    	    return 0;
    	}

        currentTime = USLOSS_Clock();

        while (sleepingHead != NULL && sleepingHead->timeToWakeUp < currentTime) {
            toWakeUp = removeFromSleepingQueue();
            toWakeUp->status = NOT_USED;
            MboxSend(toWakeUp->mailbox, NULL, 0);
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
    int currentTrack, requestType, currentSector, zapped;
    int numSectors, unit, status;
    void *buffStart;
    diskRequestPtr request;

    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    unit = atoi((char *)arg);
    currentTrack = -1;

    /* while loop to fill disk driver requests */
    while (!isZapped()) {
        zapped = MboxReceive(diskMailboxes[unit], NULL, 0);
        if (zapped == -3) {
            quit(0);
        }

        request = removeFromDiskQueue(unit);

        /* if we have nothing to do we go back to loop conditional */
        if (request == NULL) {
            continue;
        }

        /* initialize information taken from the current disk request */
        diskCurrentTrack[unit] = request->track;
        currentSector = request->startSector;
        numSectors = request->numSectors;
        requestType = request->type;


        /* loop through the number of sectors we need to read/write */
        for (int i = 0; i < numSectors; i++) {
            
            /* Check if we need to continue on to the next track */
            if (currentSector >= USLOSS_DISK_TRACK_SIZE) {
                diskCurrentTrack[unit]++;
                currentSector = 0;
            }

            /* Check if we are in the correct track. If error occurs break */
            status = checkTrack(&currentTrack, diskCurrentTrack[unit], unit);
            if (status != USLOSS_DEV_READY) {
                USLOSS_Console("DiskDriver(): error occured checking track. status %d\n", status);
                break;
            }

            /* offset the buffer to the correct position */
            buffStart = (char *)request->buffer + (512 * i);

            /* send disk request */
            status = runDiskRequest(unit, requestType, currentSector, buffStart);
            if (status != USLOSS_DEV_READY) {
                USLOSS_Console("DiskDriver(): error occured running request. status %d\n", status);
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
    first = args->arg4;
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
    if (verifyDiskParameters(dbuff, unit, first, track)) {
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
    sectors = args->arg2;
    track = args->arg3;
    first = args->arg4;
    unit = args->arg5;

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
    if (verifyDiskParameters(dbuff, unit, first, track)) {
        return -1;
    }

    int procIndex =  getpid() % MAXPROC;

    /* create and fill our request struct */
    diskRequestPtr request = &diskRequests[procIndex];
    request->type = USLOSS_DISK_WRITE;
    request->buffer = dbuff;
    request->track = track;
    request->startSector = first;
    request->numSectors = sectors;

    /* put the request on the appropriate queue */
    addToDiskQueue(unit, request);

    // wake up disk driver 
    MboxSend(diskMailboxes[unit], NULL, 0);

    // block current process
    MboxReceive(request->mailbox, NULL, 0);

    return request->result;

}

int verifyDiskParameters(void *dbuff, int unit, int first, int track)
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

    if (track < 0 || track > (unit ? disk1Tracks : disk0Tracks)) {
        return 1;
    }

    return 0;
}


void diskSize(systemArgs *args)
{
    int sector, track, disk, unit;

    unit = args->arg1;
    args->arg4 = diskSizeReal(unit, &sector, &track, &disk);

    args->arg1 = sector;
    args->arg2 = track;
    args->arg3 = disk;
}


int diskSizeReal(int unit, int *sector, int *track, int *disk)
{
    /* check if we have a correct unit */
    if (unit != DISK0 && unit != DISK1) {
        return -1;
    }
   
    *sector = USLOSS_DISK_SECTOR_SIZE;
    *track = USLOSS_DISK_TRACK_SIZE;
    *disk = unit ? disk1Tracks : disk0Tracks;

    return 0;
}

static int TerminalDriver(char *arg)
{
    int unit = atoi((char *)arg);
    int waitStatus, inputStatus, zapped, byteForWriting, writeMailboxStatus;
    char xmitStatus, recvStatus, character, charToWrite;

    byteForWriting = 1;

    /* turn on interrupts for reading and writing to the terminal */
    turnTerminalReadInterruptsOn(unit);

    while (!isZapped()) {
        zapped = waitDevice(USLOSS_TERM_DEV, unit, &waitStatus);
        if (zapped == -1) {
            return 1;
        }

        /* get the status register for the terminal */
        USLOSS_DeviceInput(USLOSS_TERM_DEV, unit, &inputStatus);
        xmitStatus = USLOSS_TERM_STAT_XMIT(inputStatus);
        recvStatus = USLOSS_TERM_STAT_RECV(waitStatus);

        if (byteForWriting) {
            writeMailboxStatus = MboxCondReceive(terminalWriteCharMailbox[unit], &charToWrite, 1);

            if (writeMailboxStatus > 0) {
                byteForWriting = 0;
            }
        }

        if (xmitStatus == USLOSS_DEV_ERROR || recvStatus == USLOSS_DEV_ERROR) {
            USLOSS_Console("TerminalDriver: waitDevice returned an error.\n");
            USLOSS_Halt(-1);
        }

        if (recvStatus == USLOSS_DEV_BUSY) {
            character = USLOSS_TERM_STAT_CHAR(inputStatus);
            MboxSend(termInMailboxes[unit], &character, 1);
        }

        /* if you are ready to write character and you have a character */
        if (xmitStatus == USLOSS_DEV_READY && byteForWriting == 0) {
            runTerminalRequest(unit, charToWrite);
            byteForWriting = 1;
        }
    }

    return 1;
}


 static int TermReader(char *arg)
 {
    int currentBufferIndex, numCharsRead, sendResult, unit, zapped;
    char buffer[11][MAXLINE];
    char character;

    currentBufferIndex = numCharsRead = 0;
    unit = atoi((char *)arg);

    while (!isZapped()) {
        
        /* receive a character and insert it into our buffer */
        zapped = MboxReceive(termInMailboxes[unit], &character, 1);
        if (zapped == -3) {
            break;
        }

        buffer[currentBufferIndex][numCharsRead++] = character;
        
        /* check if we have read a line of text yet */
        if (character == '\n' || numCharsRead == MAXLINE) {            
            sendResult = MboxCondSend(termLineOutMailboxes[unit], buffer[currentBufferIndex], numCharsRead);            

            numCharsRead = 0;
            if (sendResult == 0) {
                currentBufferIndex++;
            }
        }
    }

    return 1;
 }


void termRead(systemArgs *args)
{
    int bytesRead;
    char *buffer = args->arg1;
    int bufSize = args->arg2;
    int unit = args->arg3;

    bytesRead = termReadReal(buffer, bufSize, unit);

    args->arg2 = bytesRead;
    args->arg4 = bytesRead == -1 ? -1 : 0;
}


int termReadReal(char *buff, int bsize, int unit)
{
    char tempBuff[MAXLINE];
    int bytesRead;

    if (buff == NULL) {
        return -1;
    }

    if (unit < 0 || unit >= USLOSS_TERM_UNITS || bsize < 0) {
        return -1;
    }

    /* receive a line of input and return the number of bytes written */
    bytesRead = MboxReceive(termLineOutMailboxes[unit], tempBuff, MAXLINE);

    if (bsize < bytesRead) {
        bytesRead = bsize;
    }

    memcpy(buff, tempBuff, bytesRead);
    return bytesRead;
}


static int TermWriter(char *arg)
{
    int unit, zapped, buffSize;
    char *buff, newline;
    termRequestPtr newRequest;

    unit = atoi((char *)arg);
    newline = '\n';

    while (!isZapped()) {
        zapped = MboxReceive(terminalWriteMailbox[unit], NULL, 0);
        newRequest = removeFromTerminalWriteQueue(unit);
        if (zapped == -3) {
            return 1;
        }

        int bytesWritten = 0;
        buff = newRequest->buffer;
        buffSize = newRequest->size;

        for (int index = 0; index < buffSize; index++, bytesWritten++) {
            /* send char over to terminal driver */
            turnTerminalWriteandReadInterruptsOn(unit);
            MboxSend(terminalWriteCharMailbox[unit], (buff + index), 1);
            turnTerminalReadInterruptsOn(unit);
        }

        if (buffSize < MAXLINE - 1 && buff[buffSize-1] != '\n') {
            MboxSend(terminalWriteCharMailbox[unit], &newline, 1);
            bytesWritten++;
        }

        /* turn off terminal xmit interrupts */
        newRequest->bytesWritten = bytesWritten;
        MboxSend(newRequest->mailbox, NULL, 0);
    }

    return 1;
}


void termWrite(systemArgs *args)
{
    int bytesWritten;
    char *buffer = args->arg1;
    int bufSize = args->arg2;
    int unit = args->arg3;

    bytesWritten = termWriteReal(buffer, bufSize, unit);

    args->arg2 = bytesWritten;
    args->arg4 = bytesWritten == -1 ? -1 : 0;
}


int termWriteReal(char *buff, int bsize, int unit)
{

    int processIndex;
    termRequestPtr newRequest;

    if (buff == NULL) {
        return -1;
    }

    if (unit < 0 || unit >= USLOSS_TERM_UNITS || bsize < 0) {
        return -1;
    }

    processIndex = getpid() % MAXPROC;
    newRequest = &termWriteRequestsTable[processIndex];
    newRequest->buffer = buff;
    newRequest->size = bsize;

    /* add request to the write que and wake up terminal reader */
    addToTerminalWriteQueue(unit, newRequest);
    MboxSend(terminalWriteMailbox[unit], NULL, 0);

    /* put process to sleep */
    MboxReceive(newRequest->mailbox, NULL, 0);

    return newRequest->bytesWritten;
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

    if (temp->timeToWakeUp > toAdd->timeToWakeUp) {
        toAdd->next = temp;
        sleepingHead = toAdd;
        return;
    }

    while (temp->next != NULL && temp->next->timeToWakeUp < toAdd->timeToWakeUp ) {
        temp = temp->next;
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
    /* send to mailbox for mutex */
    MboxSend(diskQueMutex[unit], NULL, 0);

    int requestTrack;
    diskRequest proxy;
    diskRequestPtr walk;

    requestTrack = request->track;
    request->next = NULL;

    /* create proxy request node for current track number */
    proxy.track = diskCurrentTrack[unit];
    proxy.next = disksRequestQueue[unit];
    walk = &proxy;

    /* continue until request node "fits" inside two adjacent nodes */
    while (walk->next != NULL &&
            !(walk->track < requestTrack && requestTrack < walk->next->track)) {

        /* edge case where the next node is less than current node track value */
        if ((walk->track > walk->next->track) &&
                (requestTrack > walk->track || requestTrack < walk->next->track)) {
            break;
        }

        walk = walk->next;
    }

    request->next = walk->next;
    walk->next = request;
    disksRequestQueue[unit] = proxy.next;
    
    /* unblock any process that is blocked on a send */
    MboxReceive(diskQueMutex[unit], NULL, 0);
}


diskRequestPtr removeFromDiskQueue(int unit)
{
    diskRequestPtr temp = disksRequestQueue[unit];

    if (temp == NULL) {
        return NULL;
    }

    disksRequestQueue[unit] = temp->next;
    return temp;
}


void addToTerminalWriteQueue(int unit, termRequestPtr newRequest)
{
    MboxSend(terminalRequestQueMutex[unit], NULL, 0);

    termRequestPtr walk;
    termRequest temp;

    temp.next = termWriteRequests[unit];
    walk = &temp;

    while (walk->next != NULL) {
        walk = walk->next;
    }

    newRequest->next = NULL;
    walk->next = newRequest;
    termWriteRequests[unit] = temp.next;

    MboxReceive(terminalRequestQueMutex[unit], NULL, 0);
}


termRequestPtr removeFromTerminalWriteQueue(int unit)
{
    termRequestPtr temp = termWriteRequests[unit];

    if (temp == NULL) {
        return NULL;
    }

    termWriteRequests[unit] = temp->next;
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
        status = runDiskRequest(diskNumber, USLOSS_DISK_SEEK, track, 0);

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


int runTerminalRequest(int unit, char charToWrite)
{
    int newControlRegister;

    newControlRegister = USLOSS_TERM_CTRL_XMIT_CHAR(USLOSS_TERM_CTRL_RECV_INT(0));
    newControlRegister = USLOSS_TERM_CTRL_CHAR(newControlRegister, charToWrite);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, newControlRegister);
}


void checkDeviceStatus(int status, char *name)
{
    if (status != USLOSS_DEV_READY) {
        USLOSS_Console("status: %d\n", status);
        USLOSS_Console("%s: error in finding size of disk. Halting...", name);
        USLOSS_Halt(1);
    }
}


int runRequest(int typeDevice, int deviceNum, int operation, void *reg1, void *reg2)
{
    int status;
    USLOSS_DeviceRequest deviceRequest;

    fillDeviceRequest(&deviceRequest, operation, reg1, reg2);
    status = USLOSS_DeviceOutput(typeDevice, deviceNum, &deviceRequest);
    waitDevice(typeDevice, deviceNum, &status);

    return status;
}


void checkForkReturnValue(int pid, int unit, char *name)
{
    if (pid < 0) {
        USLOSS_Console("%s %d\n", name, unit);
        USLOSS_Halt(1);
    }
}


int turnTerminalReadInterruptsOn(int unit)
{
    int termInterruptsOn;

    termInterruptsOn = USLOSS_TERM_CTRL_RECV_INT(0);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, termInterruptsOn);

    return termInterruptsOn;
}


int turnTerminalWriteandReadInterruptsOn(int unit)
{
    int termInterruptsOn;

    termInterruptsOn = USLOSS_TERM_CTRL_RECV_INT(0);
    termInterruptsOn = USLOSS_TERM_CTRL_XMIT_INT(termInterruptsOn);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, termInterruptsOn);

    return termInterruptsOn;
}








