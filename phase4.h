/*
 * These are the definitions for phase 4 of the project (support level, part 2).
 */

#ifndef _PHASE4_H
#define _PHASE4_H

#include <phase2.h>

/*
 * Maximum line length
 */

#define MAXLINE         80

/* type of list definitions */
#define SLEEP_LIST  1

/* status definitions */
#define NOT_USED    0
#define IN_USE      1

/* disks */
#define DISK0       0
#define DISK1       1

/* typedef */
typedef struct proc proc;
typedef struct proc *procPtr;
typedef struct diskRequest diskRequest;
typedef struct diskRequest *diskRequestPtr;


/* structures */
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


/* helper function */
extern  void addToSleepingQueue(procPtr toAdd);
extern  procPtr removeFromSleepingQueue();
extern  int verifyDiskParameters(void *dbuff, int unit, int first, int track);
extern  void addToDiskQueue(int unit, diskRequestPtr request);
extern  diskRequestPtr removeFromDiskQueue(int unit);
extern  void fillDeviceRequest(USLOSS_DeviceRequest *request, int opr, void *reg1, void *reg2);
extern  void getAmountOfTracks();
extern  int checkTrack(int *currentTrack, int track, int diskNumber);
extern  int runDiskRequest(int diskNumber, int operation, void *reg1, void *reg2);
extern  void checkDeviceStatus(int status, char *name);
extern  int runRequest(int typeDevice, int deviceNum, int operation, void *reg1, void *reg2);
extern  void printqueSize(int unit);

/* Function prototypes for this phase */
extern  void sleep(systemArgs *args);
extern  int  sleepReal(int seconds);

extern  void diskRead(systemArgs *args);
extern  int  diskReadReal(void *dbuff, int unit, int track, int first, int sectors);

extern  void diskWrite(systemArgs *args);
extern  int  diskWriteReal(void *dbuff, int unit, int track, int first, int sectors);

extern  void diskSize(systemArgs *args);
extern  int  diskSizeReal(int unit, int *sector, int *track, int *disk);

extern  void termRead(systemArgs *args);
extern  int  termReadReal(char *buff, int bsize, int unit_id, int *nread);

extern  void termWrite(systemArgs *args);
extern  int  termWriteReal(char *buff, int bsize, int unit_id, int *nwrite);

extern  int  start4(char *);

#define ERR_INVALID             -1
#define ERR_OK                  0

#endif /* _PHASE4_H */
