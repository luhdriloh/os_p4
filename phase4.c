#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <providedPrototypes.h>


#include <stdlib.h> /* needed for atoi() */
#include <stdio.h>
#include <string.h> /* needed for memcpy() */
    
/* Sleeping queue */
proc processTable[MAXPROC];
procPtr sleepingHead;

/* Disk request queues and mailboxes */
diskRequestPtr disksRequestQueue[USLOSS_DISK_UNITS];
int diskMailboxes[USLOSS_DISK_UNITS];
int diskCurrentTrack[USLOSS_DISK_UNITS];
int diskQueMutex[USLOSS_DISK_UNITS];

/* Array of disk requests */
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
int termWriterMailbox[USLOSS_TERM_UNITS];
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
    int     pid;
    int     status;
    int		diskPID[USLOSS_DISK_UNITS];
    int     termDriverPID[USLOSS_TERM_UNITS];


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
     */
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    checkForkReturnValue(clockPID, 0, "start3(): Can't create clock driver unit ");


    /*
     * Create the disk device drivers
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
    
    /* Create terminal device drivers */
    for (int unit = 0; unit < USLOSS_TERM_UNITS; unit++) {
        
        /* create mailboxes associated with each terminal */
        termInMailboxes[unit] = MboxCreate(0, 1);
        termLineOutMailboxes[unit] = MboxCreate(LINE_BUFFER_SIZE, MAXLINE);
        terminalWriteCharMailbox[unit] = MboxCreate(0, 1);
        terminalRequestQueMutex[unit] = MboxCreate(1, 0);
        termWriterMailbox[unit] = MboxCreate(MAXPROC, 0);

        sprintf(buf, "%d", unit);

        /* Create the terminal driver process */
        sprintf(name, "Terminal driver %d", unit);
        pid = fork1(name, TerminalDriver, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal driver unit ");
        termDriverPID[unit] = pid;

        /* Create the terminal reader process */
        sprintf(name, "Terminal reader %d", unit);
        pid = fork1(name, TermReader, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal reader unit ");

        /* Create the terminal writer process */
        sprintf(name, "Terminal writer %d", unit);
        pid = fork1(name, TermWriter, buf, USLOSS_MIN_STACK, 2);
        checkForkReturnValue(pid, unit, "start3(): Can't create terminal writer unit ");
    }


    /* Spawn user level process */
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
        MboxRelease(termWriterMailbox[unit]);

        turnTerminalWriteandReadInterruptsOn(unit);
        zap(termDriverPID[unit]);
    }

    quit(0);
}


/* ------------------------------------------------------------------------
   Name - ClockDriver
   Purpose - Provides the sleeping functionality for processes that ask
                to be put to sleep. 
   Parameters - 
            char *arg: Holds the unit number of the clock driver
   Returns - 
            int      : A return status 
   Side Effects - Will unblock processes that need to waken up
   ----------------------------------------------------------------------- */

static int ClockDriver(char *arg)
{
    int result, status, currentTime;
    procPtr toWakeUp;

    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    /* Loop to check if it is ready to unblock any sleeping processes */
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


/* ------------------------------------------------------------------------
   Name - sleep
   Purpose - Parses out the systemArgs that was passed in by the user process
                Sleep
   Parameters -
            systemArgs *args : Holds parameters inside used for sleepReal
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

void sleep(systemArgs *args)
{
    int seconds;
    seconds = args->arg1;
    args->arg4 = sleepReal(seconds);
}


/* ------------------------------------------------------------------------
   Name - sleepReal
   Purpose - Prepares a process to be put to sleep
   Parameters - 
            int seconds : The number of the seconds the process is to be
                put to sleep 
   Returns - 
            int : Integer indicating success 
   Side Effects - Puts a process to sleep
   ----------------------------------------------------------------------- */

int sleepReal(int seconds)
{
    if (seconds < 0) {
        return -1;
    }

    int pid, sleepProcTableIndex, timeToWakeUp;
    procPtr sleepProc;

    pid = getpid();
    sleepProcTableIndex = pid % MAXPROC;
    timeToWakeUp = USLOSS_Clock() + (seconds * 1000000);
    sleepProc = &processTable[sleepProcTableIndex];

    if (sleepProc->status != NOT_USED) {
        return -1;
    }

    /* Fill sleep request struct */
    sleepProc->pid = pid;
    sleepProc->timeToWakeUp = timeToWakeUp;
    sleepProc->next = NULL;
    sleepProc->status = IN_USE;

    addToSleepingQueue(sleepProc);
    
    /* block process */
    MboxReceive(sleepProc->mailbox, NULL, 0);

    return 0;
}


/* ------------------------------------------------------------------------
   Name - DiskDriver
   Purpose - Fullfill user disk requests
   Parameters -
            char *arg : The unit number of the disk driver
   Returns -
            int       : Integer indicating success
   Side Effects - Fullfills disk requests and also unblocks a process
                    waiting for a result
   ----------------------------------------------------------------------- */

static int DiskDriver(char *arg)
{
    int currentTrack, requestType, currentSector, mailboxStatus;
    int numSectors, unit, status;
    void *buffStart;
    diskRequestPtr request;

    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    unit = atoi((char *)arg);
    currentTrack = -1;

    /* Loop to fullfill disk requests */
    while (!isZapped()) {
        mailboxStatus = MboxReceive(diskMailboxes[unit], NULL, 0);
        if (mailboxStatus == MAILBOX_RELEASED) {
            return 1;
        }

        /* Get next request */
        request = removeFromDiskQueue(unit);

        /* Initialize information taken from the current disk request */
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

            /* Offset the buffer to the correct position */
            buffStart = (char *)request->buffer + (512 * i);

            /* Send disk request */
            status = runDiskRequest(unit, requestType, currentSector, buffStart);
            if (status != USLOSS_DEV_READY) {
                USLOSS_Console("DiskDriver(): error occured running request. status %d\n", status);
                break;
            }

            currentSector++;
        }

        /* Release the mailbox with the status returned */
        request->result = status;
        MboxSend(request->mailbox, NULL, 0);
    }

    return 0;
}


/* ------------------------------------------------------------------------
   Name - diskRead
   Purpose - Parses out the systemArgs that was passed in by the user process
                DiskRead
   Parameters -
            systemArgs *args : Holds parameters inside used for diskReadReal
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - diskReadReal
   Purpose - Prepares a read request for a process
   Parameters - 
        void *dbuff : The buffer to put the read in information in
        int  unit   : The disk unit to read from
        int  track  : The track to read from
        int  first  : First track to read from
        int  sectors: Amount of tracks to read
   Returns - 
            int         : Integer indicating success 
   Side Effects - Puts information read from disk into dbuff and puts
                    process to sleep until read is complete
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - diskWrite
   Purpose - Parses out the systemArgs that was passed in by the user process
                DiskWrite
   Parameters -
            systemArgs *args : Holds parameters inside used for diskWriteReal
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------
   Name - diskWriteReal
   Purpose - Prepares a disk write request for a process
   Parameters - 
        void *dbuff : The buffer that has the data to write
        int  unit   : The disk unit to write to
        int  track  : The track to write to
        int  first  : First track to write to
        int  sectors: Amount of tracks to write
   Returns - 
            int         : Integer indicating success 
   Side Effects - Puts information read from disk into dbuff and puts
                    process to sleep until read is complete
   ----------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------
   Name - diskSize
   Purpose - Parses out the systemArgs that was passed in by the user process
                DiskSize
   Parameters -
            systemArgs *args : Holds parameters used for diskSize
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

void diskSize(systemArgs *args)
{
    int sector, track, disk, unit;

    unit = args->arg1;
    args->arg4 = diskSizeReal(unit, &sector, &track, &disk);

    args->arg1 = sector;
    args->arg2 = track;
    args->arg3 = disk;
}


/* ------------------------------------------------------------------------
   Name - diskSizeReal
   Purpose - Prepares a disk size request for a process
   Parameters - 
        int  unit    : The disk unit to get the size of
        int  *track  : Pointer to put amount of secors in track
        int  *disk   : Pointer to put amount of tracks in a disk
        int  *sector : Pointer to put amount of btyes in a sector
   Returns - 
            int         : Integer indicating success 
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------
   Name - verifyDiskParameters
   Purpose - Verifies disk paramters
   Parameters - Parameters are those passed to diskSize, diskRead,
                    and diskWrite
   Returns - An integer indicating whether you have invalid disk parameters
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - TerminalDriver
   Purpose - Fullfill user terminal requests
   Parameters -
            char *arg : The unit number of the terminal driver
   Returns -
            int       : Integer indicating success
   Side Effects - Fullfills terminal requests and unblocks processes
                    waiting for a result
   ----------------------------------------------------------------------- */

static int TerminalDriver(char *arg)
{
    int unit = atoi((char *)arg);
    int waitStatus, mailboxStatus, writeMailboxStatus;
    char xmitStatus, recvStatus, charToRead, charToWrite;


    /* turn on interrupts for reading from the terminal */
    turnTerminalReadInterruptsOn(unit);

    while (!isZapped()) {
        mailboxStatus = waitDevice(USLOSS_TERM_DEV, unit, &waitStatus);
        if (mailboxStatus == ZAPPED) {
            return 1;
        }

        /* Get the status for xmit and recv */
        xmitStatus = USLOSS_TERM_STAT_XMIT(waitStatus);
        recvStatus = USLOSS_TERM_STAT_RECV(waitStatus);

        if (xmitStatus == USLOSS_DEV_ERROR || recvStatus == USLOSS_DEV_ERROR) {
            USLOSS_Console("TerminalDriver: waitDevice returned an error.\n");
            USLOSS_Halt(-1);
        }

        /* Receive the character that is in the register */
        if (recvStatus == USLOSS_DEV_BUSY) {
            charToRead = USLOSS_TERM_STAT_CHAR(waitStatus);
            MboxSend(termInMailboxes[unit], &charToRead, 1);
        }

        /* Write a character to the terminal if you are ready */
        if (xmitStatus == USLOSS_DEV_READY) {
            writeMailboxStatus = MboxCondReceive(terminalWriteCharMailbox[unit], &charToWrite, 1);

            /*  Check that we have a character to write */
            if (writeMailboxStatus > 0) {
                runTerminalRequest(unit, charToWrite);  
                turnTerminalReadInterruptsOn(unit);              
            }
        }
    }

    return 1;
}


/* ------------------------------------------------------------------------
   Name - TermReader
   Purpose - Fullfill user terminal read requests
   Parameters -
            char *arg : The unit number of the terminal reader
   Returns -
            int       : Integer indicating success
   Side Effects - Fullfills terminal read requests and unblocks processes
                    waiting for a result
   ----------------------------------------------------------------------- */

 static int TermReader(char *arg)
 {
    int currentBufferIndex, numCharsRead, sendResult, unit, mailboxStatus;
    char buffer[11][MAXLINE];
    char character;

    currentBufferIndex = numCharsRead = 0;
    unit = atoi((char *)arg);

    while (!isZapped()) {
        /* receive a character and insert it into our buffer */
        mailboxStatus = MboxReceive(termInMailboxes[unit], &character, 1);
        if (mailboxStatus == MAILBOX_RELEASED) {
            break;
        }

        /* Insert character into line buffer */
        buffer[currentBufferIndex][numCharsRead++] = character;
        
        /*
         * Check if we have read a line of input yet. If so send it to the
         * termLineOutMailbox for termReadReal to read from.
         */
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


/* ------------------------------------------------------------------------
   Name - termRead
   Purpose - Parses out the systemArgs that was passed in by the user process
                TermRead
   Parameters -
            systemArgs *args : Holds parameters inside used for termRead
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - termReadReal
   Purpose - Prepares a terminal read request for a process
   Parameters - 
        char *buff   : The buffer to put in read in information into
        int  bsize   : Size of the buffer, as to not write to much into it
        int  unit    : The terminal unit to read from
   Returns - 
            int         : Integer indicating success 
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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

    /* Find the appropriate amount of characters to write to buffer */
    if (bsize < bytesRead) {
        bytesRead = bsize;
    }

    memcpy(buff, tempBuff, bytesRead);
    return bytesRead;
}


/* ------------------------------------------------------------------------
   Name - TermWriter
   Purpose - Fullfill user terminal write requests
   Parameters -
            char *arg : The unit number of the terminal driver
   Returns -
            int       : Integer indicating success
   Side Effects - Fullfills terminal write requests and unblocks processes
                    waiting for a result
   ----------------------------------------------------------------------- */

static int TermWriter(char *arg)
{
    int unit, mailboxStatus, buffSize, bytesWritten;
    char *buff, newline;
    termRequestPtr newRequest;

    unit = atoi((char *)arg);
    newline = '\n';

    while (!isZapped()) {
        mailboxStatus = MboxReceive(termWriterMailbox[unit], NULL, 0);
        if (mailboxStatus == MAILBOX_RELEASED) {
            return 1;
        }

        /* Receive next request from queue */
        newRequest = removeFromTerminalWriteQueue(unit);
        buff = newRequest->buffer;
        buffSize = newRequest->size;

        for (bytesWritten = 0; bytesWritten < buffSize; bytesWritten++) {
            /* send char over to terminal driver */
            turnTerminalWriteandReadInterruptsOn(unit);
            MboxSend(terminalWriteCharMailbox[unit], (buff + bytesWritten), 1);
        }

        if (buffSize < MAXLINE - 1 && buff[buffSize-1] != '\n') {
            turnTerminalWriteandReadInterruptsOn(unit);
            MboxSend(terminalWriteCharMailbox[unit], &newline, 1);
            bytesWritten++;
        }

        /* unblock the process that asked for a write */
        newRequest->bytesWritten = bytesWritten;
        MboxSend(newRequest->mailbox, NULL, 0);
    }

    return 1;
}


/* ------------------------------------------------------------------------
   Name - termWrite
   Purpose - Parses out the systemArgs that was passed in by the user process
                TermWrite
   Parameters -
            systemArgs *args : Holds parameters inside used for termWrite
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - termWriteReal
   Purpose - Prepares a terminal write request for a process
   Parameters - 
        char *buff   : The buffer to write from
        int  bsize   : Size of the buffer, as to write the right amount
                            characters to the terminal
        int  unit    : The terminal unit to read from
   Returns - 
            int         : Integer indicating success 
   Side Effects - n/a
   ----------------------------------------------------------------------- */

int termWriteReal(char *buff, int bsize, int unit)
{

    int processIndex;
    termRequestPtr newRequest;

    /* Error checking */
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
    MboxSend(termWriterMailbox[unit], NULL, 0);

    /* put process to sleep */
    MboxReceive(newRequest->mailbox, NULL, 0);

    return newRequest->bytesWritten;
}


/* ------------------------------------------------------------------------
   Name - addToSleepingQueue
   Purpose - Add a process to the sleeping queue
   Parameters - 
            procPtr toAdd: A pointer to the process to add to the queue
   Returns - n/a
   Side Effects -
            Adds a process to the global procPtr 'sleepingHead', which
                holds the processes needing to sleep for a certain amount
                of time
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
   Name - removeFromSleepingQueue
   Purpose - Removes the first element from the sleeping queue
   Parameters - n/a
   Returns - 
            procPtr : A pointer to the removed procPtr 
   Side Effects - Removes a procPtr from the list 'sleepingHead'
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


/* ------------------------------------------------------------------------
   Name - addToDiskQueue
   Purpose - Add a disk request to the appropriate disk request head
   Parameters -
            int unit : The disk driver unit to add the request to
            diskRequestPtr request : The actual request to add to
                the disk request queue 'disksRequestQueue[unit]'
   Returns - n/a
   Side Effects - A request is added the queue
   ----------------------------------------------------------------------- */

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

    /* Add the new request to the queue */
    request->next = walk->next;
    walk->next = request;

    /* Set the new head */
    disksRequestQueue[unit] = proxy.next;
    
    /* unblock any process that is blocked on a send */
    MboxReceive(diskQueMutex[unit], NULL, 0);
}


/* ------------------------------------------------------------------------
   Name - removeFromDiskQueue
   Purpose - Remove the first request from the appropriate disk request
                queue 'disksRequestQueue[unit]'
   Parameters -
            int unit : The unit from which to remove a request from
   Returns - 
            diskRequestPtr : A pointer to the removed request
   Side Effects - The request queue is decreased as an element is removed
   ----------------------------------------------------------------------- */

diskRequestPtr removeFromDiskQueue(int unit)
{
    diskRequestPtr temp = disksRequestQueue[unit];

    if (temp == NULL) {
        return NULL;
    }

    disksRequestQueue[unit] = temp->next;
    return temp;
}


/* ------------------------------------------------------------------------
   Name - addToTerminalWriteQueue
   Purpose - Add a terminal write request to the appropriate
                terminal write request head pointer 'termWriteRequests[unit]'
   Parameters -
            int unit : The disk driver unit to add the request to
            termRequestPtr request : The actual request to add to
                the terminal write request queue 'termWriteRequests[unit]'
   Returns - n/a
   Side Effects - A request is added to the terminalWriteRequests head
   ----------------------------------------------------------------------- */

void addToTerminalWriteQueue(int unit, termRequestPtr request)
{
    MboxSend(terminalRequestQueMutex[unit], NULL, 0);

    termRequestPtr walk;
    termRequest temp;

    temp.next = termWriteRequests[unit];
    walk = &temp;

    while (walk->next != NULL) {
        walk = walk->next;
    }

    request->next = NULL;
    walk->next = request;
    termWriteRequests[unit] = temp.next;

    MboxReceive(terminalRequestQueMutex[unit], NULL, 0);
}


/* ------------------------------------------------------------------------
   Name - removeFromTerminalWriteQueue
   Purpose - Remove the first request from the appropriate terminal write
                request queue 'terminalWriteRequests[unit]'
   Parameters -
            int unit : The unit from which to remove a request from
   Returns - 
            termRequestPtr : A pointer to the removed request
   Side Effects - The request queue is decreased as an element is removed
   ----------------------------------------------------------------------- */

termRequestPtr removeFromTerminalWriteQueue(int unit)
{
    termRequestPtr temp = termWriteRequests[unit];

    if (temp == NULL) {
        return NULL;
    }

    termWriteRequests[unit] = temp->next;
    return temp;
}


/* ------------------------------------------------------------------------
   Name - fillDeviceRequest
   Purpose - Fill a USLOSS_DeviceRequest struct
   Parameters -
        USLOSS_DeviceRequest *request : Pointer to request to fill in
        int opr : Type of request
        void *reg1 : Parameter one to put into the request
        void *reg2 : Parameter two to put into the request

   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

void fillDeviceRequest(USLOSS_DeviceRequest *request, int opr, void *reg1, void *reg2)
{
    request->opr = opr;
    request->reg1 = reg1;
    request->reg2 = reg2;
}


/* ------------------------------------------------------------------------
   Name - getAmountOfTracks
   Purpose - Gets the amount of tracks in a disk for use in diskSize
   Parameters - n/a
   Returns - n/a
   Side Effects - Sets global variables disk0Tracks, and disk1Tracks with
                    correct amount of tracks within each disk
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - checkTrack
   Purpose - Changes the track that the disk is on
   Parameters -
        int *currentTrack : The value of the current track we are on
        int track : The value of the track that we should be on
        int diskNumber : The unit of the disk we are looking at
   Returns - n/a
   Side Effects - If a change in track occurs then currentTrack is changed
                    to the new track we are on
   ----------------------------------------------------------------------- */

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


/* ------------------------------------------------------------------------
   Name - runDiskRequest
   Purpose - Run a disk request
   Parameters -
        int diskNumber : The disk number to run the request on
        int operation : The operation we want to run on the disk
        void *reg1 : The first parameter for our USLOSS_DeviceRequest struct
        void *reg2 : The second parameter for our USLOSS_DeviceRequest struct
   Returns -
        int : Integer indicating success
   Side Effects - A disk request will have been run unless an error occurred
   ----------------------------------------------------------------------- */

int runDiskRequest(int diskNumber, int operation, void *reg1, void *reg2)
{
    return runRequest(USLOSS_DISK_DEV, diskNumber, operation, reg1, reg2);
}


/* ------------------------------------------------------------------------
   Name - runTerminalRequest
   Purpose - Runs a terminal request that writes something to the terminal
   Parameters -
            int unit : The terminal unit on which to run the request
            char charToWrite : The character to write to the terminal
   Returns -
            int : Integer indicating success
   Side Effects - The specified terminal will have been operated upon
                    unless an error is returned
   ----------------------------------------------------------------------- */

int runTerminalRequest(int unit, char charToWrite)
{
    int newControlRegister;

    newControlRegister = USLOSS_TERM_CTRL_XMIT_CHAR(USLOSS_TERM_CTRL_RECV_INT(0));
    newControlRegister = USLOSS_TERM_CTRL_CHAR(newControlRegister, charToWrite);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, newControlRegister);
}


/* ------------------------------------------------------------------------
   Name - checkDeviceStatus
   Purpose - Checks status of a device. Used for the initial setup of the
                diskDrivers
   Parameters -
            int status : The status that was returned from the device
            char *name : The name of the function who called this
   Returns - n/a
   Side Effects - Halts since this needs to be valid for diskSize to work
                    correctly
   ----------------------------------------------------------------------- */

void checkDeviceStatus(int status, char *name)
{
    if (status != USLOSS_DEV_READY) {
        USLOSS_Console("status: %d\n", status);
        USLOSS_Console("%s: error in finding size of disk. Halting...", name);
        USLOSS_Halt(1);
    }
}


/* ------------------------------------------------------------------------
   Name - runRequest
   Purpose - Calls USLOSS_DeviceOutput and waits for it to complete an action
   Parameters -
            int typeDevice : The type of device to run a request on
            int deviceNum : The unit of the device to run request on
            int operation : The type of operation to run on the specified device
            void *reg1 : Parameter one to put into the deviceRequest struct
            void *reg2 : Parameter two to put into the deviceRequest struct
   Returns - n/a
   Side Effects - n/a
   ----------------------------------------------------------------------- */

int runRequest(int typeDevice, int deviceNum, int operation, void *reg1, void *reg2)
{
    int status;
    USLOSS_DeviceRequest deviceRequest;

    fillDeviceRequest(&deviceRequest, operation, reg1, reg2);
    status = USLOSS_DeviceOutput(typeDevice, deviceNum, &deviceRequest);
    waitDevice(typeDevice, deviceNum, &status);

    return status;
}


/* ------------------------------------------------------------------------
   Name - checkForkReturnValue
   Purpose - Checks that a valid pid is returned
   Parameters -
            int pid : pid to check
            int unit : Unit of device this was called on
            char *name : Name of device who called this function
   Returns - n/a
   Side Effects - Halts if error occured
   ----------------------------------------------------------------------- */

void checkForkReturnValue(int pid, int unit, char *name)
{
    if (pid < 0) {
        USLOSS_Console("%s %d: Error occured with pid returned\n", name, unit);
        USLOSS_Halt(1);
    }
}


/* ------------------------------------------------------------------------
   Name - turnTerminalReadInterruptsOn
   Purpose - Turns on receive interrupts for the terminal
   Parameters -
            int unit : Terminal unit to turn interrupts on
   Returns -
            int : New status register
   Side Effects - Turn on interrupts for a specified terminal
   ----------------------------------------------------------------------- */

int turnTerminalReadInterruptsOn(int unit)
{
    int termInterruptsOn;

    termInterruptsOn = USLOSS_TERM_CTRL_RECV_INT(0);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, termInterruptsOn);

    return termInterruptsOn;
}


/* ------------------------------------------------------------------------
   Name - turnTerminalWriteandReadInterruptsOn
   Purpose - Turns on receive and write interrupts for the terminal
   Parameters -
            int unit : Terminal unit to turn interrupts on
   Returns -
            int : New status register
   Side Effects - Turn on interrupts for a specified terminal
   ----------------------------------------------------------------------- */

int turnTerminalWriteandReadInterruptsOn(int unit)
{
    int termInterruptsOn;

    termInterruptsOn = USLOSS_TERM_CTRL_RECV_INT(0);
    termInterruptsOn = USLOSS_TERM_CTRL_XMIT_INT(termInterruptsOn);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, termInterruptsOn);

    return termInterruptsOn;
}


