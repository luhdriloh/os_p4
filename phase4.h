/*
 * These are the definitions for phase 4 of the project (support level, part 2).
 */

#ifndef _PHASE4_H
#define _PHASE4_H

/*
 * Maximum line length
 */

#define MAXLINE         80

/* type of list definitions */
#define SLEEP_LIST  1

/* status definitions */
#define NOT_USED    0
#define IN_USE      1

/* typedef */
typedef struct proc proc;
typedef struct proc *procPtr;

/* structures */
struct proc {
    int pid;
    int timeToWakeUp;
    int mailbox;

    procPtr next;
    int status;
};

/* helper function */
extern procPtr addToList(procPtr head, procPtr toAdd, int listType);
extern procPtr removeFromList(procPtr head, procPtr toRemove, int listType);

/* Function prototypes for this phase */
extern  void sleep(systemArgs *args);
extern  int  sleepReal(int seconds);

extern  void diskRead(systemArgs *args);
extern  int  diskReadReal(void *dbuff, int unit, int track, int first, int sectors,int *status);

extern  void diskWrite(systemArgs *args);
extern  int  diskWriteReal(void *dbuff, int unit, int track, int first, int sectors,int *status);

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
