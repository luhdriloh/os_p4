/*
 * These are the definitions for phase 4 of the project (support level, part 2).
 */

#ifndef _PHASE4_H
#define _PHASE4_H

#include <phase2.h>

/* Maximum line length*/
#define MAXLINE         80

/* status definitions for sleep list*/
#define NOT_USED    0
#define IN_USE      1

/* disks */
#define DISK0       0
#define DISK1       1

/* Mailbox Status */
#define MAILBOX_RELEASED -3
#define ZAPPED           -1

/* Terminal */
#define LINE_BUFFER_SIZE 10

/* Typedefs */
typedef struct proc proc;
typedef struct proc *procPtr;
typedef struct diskRequest diskRequest;
typedef struct diskRequest *diskRequestPtr;
typedef struct termRequest termRequest;
typedef struct termRequest *termRequestPtr;

/* Structures */
struct proc {
    int pid;
    int timeToWakeUp;
    int mailbox;

    procPtr next;
    int status;
};

struct diskRequest {
    int             type;
    void            *buffer;
    int             track;
    int             startSector;
    int             numSectors;
    diskRequestPtr  next;
    int             result;
    int             mailbox;
};

struct termRequest {
    void            *buffer;
    int             size;
    termRequestPtr  next;
    int             mailbox;
    int             bytesWritten;
};


/* Clock Driver Functions */
extern  void    addToSleepingQueue(procPtr toAdd);
extern  procPtr removeFromSleepingQueue();


/* Disk Driver Functions */
extern  void getAmountOfTracks();
extern  int  checkTrack(int *, int, int);
extern  int  runDiskRequest(int, int, void *, void *);
extern  int  verifyDiskParameters(void *, int, int, int);
extern  void addToDiskQueue(int, diskRequestPtr);
extern  diskRequestPtr removeFromDiskQueue(int unit);


/* Terminal Driver Functions */
extern  void addToTerminalWriteQueue(int, termRequestPtr);
extern  termRequestPtr removeFromTerminalWriteQueue(int);
extern  int runTerminalRequest(int, char);
extern  int  turnTerminalReadInterruptsOn(int);
extern  int  turnTerminalWriteandReadInterruptsOn();


/* Helper Function */
extern  void fillDeviceRequest(USLOSS_DeviceRequest *, int, void *, void *);
extern  void checkDeviceStatus(int , char *);
extern  int  runRequest(int, int, int, void *, void *);
extern  void checkForkReturnValue(int, int, char *);


/* User Function */
extern  void sleep(systemArgs *args);
extern  int  sleepReal(int seconds);

extern  void diskRead(systemArgs *args);
extern  int  diskReadReal(void *dbuff, int unit, int track, int first, int sectors);

extern  void diskWrite(systemArgs *args);
extern  int  diskWriteReal(void *dbuff, int unit, int track, int first, int sectors);

extern  void diskSize(systemArgs *args);
extern  int  diskSizeReal(int unit, int *sector, int *track, int *disk);

extern  void termRead(systemArgs *args);
extern  int  termReadReal(char *buff, int bsize, int unit_id);

extern  void termWrite(systemArgs *args);
extern  int  termWriteReal(char *buff, int bsize, int unit_id);

extern  int  start4(char *);

#define ERR_INVALID             -1
#define ERR_OK                  0

#endif /* _PHASE4_H */
