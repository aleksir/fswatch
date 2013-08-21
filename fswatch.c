#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>

#include <CoreServices/CoreServices.h> 

/**
 * fswatch.c
 * usage: ./fswatch /some/<directory|file> [/some/other<directory|file> ...] "some command"
 * "some command" is eval'd by bash when /some/directory generates any file events
 *
 * compile me with something like: gcc fswatch.c -framework CoreServices -o fswatch
 *
 * adapted from the FSEvents api PDF
 */

#define FILE_ENV_VAR "FSFILE"

// environment variables
extern char **environ;

//the command to run
char *to_run;

int kernel_queue;
// int *event_fds;
int nevents = 0;
struct kevent *events_to_monitor;
struct kevent *event_data;

unsigned int vnode_events = NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_REVOKE;

void call_with_env(char **env) {
    pid_t pid;
    int status;

    if ((pid = fork()) < 0) {
        fprintf(stderr, "error: couldn't fork \n");
        exit(1);
    } else if (pid == 0) {
        char *args[4] = {
                "/bin/bash",
                "-c",
                to_run,
                0
            };
        if (execve(args[0], args, env) < 0) {
            fprintf(stderr, "error: error executing\n");
            exit(1);
        }
    } else {
        while (wait(&status) != pid)
            ;
    }
}

//fork a process when there's any change in watched dir
void callback( 
    ConstFSEventStreamRef streamRef, 
    void *clientCallBackInfo, 
    size_t numEvents, 
    void *eventPaths, 
    const FSEventStreamEventFlags eventFlags[], 
    const FSEventStreamEventId eventIds[]) 
{ 
    // printf("Callback called\n");
    
    call_with_env(environ);
} 

/** Wathes changes in dir. */
void watch_dirs(CFArrayRef dirsToWatch) {
    void *callbackInfo = NULL; 
    FSEventStreamRef stream; 
    CFAbsoluteTime latency = 1.0;

    if (CFArrayGetCount(dirsToWatch) <= 0) { return; }

    // watch directories
    stream = FSEventStreamCreate(NULL,
            &callback,
            callbackInfo,
            dirsToWatch,
            kFSEventStreamEventIdSinceNow,
            latency,
            kFSEventStreamCreateFlagNone
        ); 

    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode); 
    FSEventStreamStart(stream);
}

/** Wathes changes in single file with kernel queues. */
void watch_files(CFArrayRef filesToWatch) {
    int event_fd;
    char *path;
    CFStringRef pathStr;
    Boolean success;

    CFIndex i, count;
    
    count = CFArrayGetCount(filesToWatch);
    nevents = count;
    events_to_monitor = malloc(sizeof(struct kevent) * count);
    event_data = malloc(sizeof(struct kevent) * count);
    
    // event_fds = malloc(sizeof(int) * count);

    if (count <= 0) { return; }

    // Open a kernel queue.
    if ((kernel_queue = kqueue()) < 0) {
        fprintf(stderr, "Could not open kernel queue.  Error was %s.\n", strerror(errno));
    }

    // vnode_events = NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_REVOKE;

    for (i=0; i<count; i++) {
        pathStr = CFArrayGetValueAtIndex(filesToWatch,i);
        
        path = NULL;
        if ((path = (char *) malloc(CFStringGetLength(pathStr)+1)) == NULL) {
            fprintf(stderr, "Could not malloc.\n");
            exit(1);
        }
        if (!CFStringGetCString(pathStr, path, CFStringGetLength(pathStr)+1, kCFStringEncodingUTF8)) {
            fprintf(stderr, "Something went wrong.\n");
        }

        event_fd = open(path, O_EVTONLY);

        if (event_fd <= 0) {
            fprintf(stderr, "The file %s could not be opened for monitoring.  Error was %s.\n", path, strerror(errno));
            exit(-1);
        }

        EV_SET( &events_to_monitor[i], event_fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, vnode_events, i, path); // path is set to data
        // EV_SET64( &events_to_monitor[i], event_fd, EVFILT_VNODE, (EV_ADD | EV_ENABLE | EV_CLEAR), vnode_events, i, (uint64_t)path, 0, 0); // path is set to data
        /*
        printf("File number: %ld\n",i);
        printf("Ident: %ld\n", events_to_monitor[i].ident);
        printf("Filter: %u\n", events_to_monitor[i].filter);
        printf("Flags: %u\n", events_to_monitor[i].flags);
        printf("Path: %s\n\n", (char *)events_to_monitor[i].udata);
        */
    }
}

/** Loop the events */
void loop() {
    if ( nevents > 0 ) {
        int event_count, i;
        struct timespec timeout;

        pid_t pid;
        int status;
        int event_fd;
        char *path;

        // Set the timeout to wake us every half second.
        timeout.tv_sec = 0;        // 0 seconds
        timeout.tv_nsec = 500000000;    // 500 milliseconds

        while (1) {
            int event_count = kevent(kernel_queue, events_to_monitor, nevents, event_data, nevents, &timeout);
            //int event_count = kevent64(kernel_queue, events_to_monitor, nevents, event_data, nevents, 0, &timeout);

            //printf("%d events.\n", event_count);
            
            if (event_count < 0 || (event_data[0].flags == EV_ERROR)) {
                // An error occurred.
                fprintf(stderr, "An error occurred (event count %d).  The error was %s.\n", event_count, strerror(errno));
                break;
            }

            for (i=0; i < event_count; i++) {

                setenv(FILE_ENV_VAR, event_data[i].udata, 1);
                // file_callback(&event_data[i]);
                call_with_env(environ);
                unsetenv(FILE_ENV_VAR);
                // printf("Executing %d of %d, filter: %u, flags: %u, pointer: %lu, data: %ld.\n", 
                //         i+1, event_count, event_data[i].filter, event_data[i].flags, event_data[i].ident, event_data[i].data);

                // try to reopen file it is deleted
                if (event_data[i].flags & NOTE_DELETE) {
                    // printf("Deleted.\n");
                    path = (char *)events_to_monitor[event_data[i].data].udata;
                    event_fd = open(path, O_EVTONLY);
                    if (event_fd > 0) {
                        // close existing handle
                        close(events_to_monitor[event_data[i].data].ident);
                        events_to_monitor[event_data[i].data].ident = event_fd;
                        event_data[i].ident = event_fd;
                    }
                }
            }

            timeout.tv_sec = 0;        // 0 seconds
            timeout.tv_nsec = 500000000;    // 500 milliseconds
        }

        for (i=0; i < nevents; i++) {
            close(events_to_monitor[i].ident);
        }
    } else {
        CFRunLoopRun();
    }
}
 
//set up fsevents and callback
int main(int argc, char **argv) {
    int i, count;

    char *path;
    struct stat fileinfo;

    CFStringRef pathStr;

    CFMutableArrayRef dirsToWatch,
                      filesToWatch;

    if (argc < 3) {
        // fprintf(stderr, "You must specify a directory to watch and a command to execute on change\n");
        fprintf(stderr, "usage: ./fswatch /some/directory [/some/otherdirectory ...] \"some command\"\n");
        exit(1);
    }

    to_run = argv[argc-1]; // last argument is a callback

    dirsToWatch  = CFArrayCreateMutable(NULL, argc-2, &kCFTypeArrayCallBacks); // stores CFStringRefs
    filesToWatch = CFArrayCreateMutable(NULL, argc-2, &kCFTypeArrayCallBacks); // stores CFStringRefs

    // check whether path is a file or a dir
    count = argc - 1; // CFArrayGetCount(pathsToWatch);

    for (i = 1; i < count; i++) {
        path = argv[i];
        pathStr = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);

        // http://pubs.opengroup.org/onlinepubs/009604599/basedefs/sys/stat.h.html
        if ( stat(path, &fileinfo) < 0 ) {
            fprintf(stderr, "Cannot get file info: %s\n", path);
            exit(1);
        }

        if (fileinfo.st_mode & S_IFDIR) {
            // printf("Dir: %s\n", path);
            CFArrayAppendValue(dirsToWatch, pathStr);
        } else if (fileinfo.st_mode & S_IFREG) {
            // printf("File: %s\n", path);
            CFArrayAppendValue(filesToWatch, pathStr);
        } 
        // TODO: symbolic links etc.

        CFRelease(pathStr); // path retained when appended into an array
    }

    watch_files(filesToWatch);
    watch_dirs(dirsToWatch);

    loop();
}
