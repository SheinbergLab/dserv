//
// Created by David Sheinberg on 2019-06-08.
//

#ifndef DSERV_DAEMONIZER_HPP
#define DSERV_DAEMONIZER_HPP

#include <sys/stat.h>

#ifndef _WIN32
#include <syslog.h>
#include <unistd.h>
#endif

class Daemonizer {

public:
    void start(void)
    {
#ifndef _WIN32
        // Define variables
        pid_t pid, sid;

        // Fork the current process
        pid = fork();
        // The parent process continues with a process ID greater than 0
        if (pid > 0) {
            exit(EXIT_SUCCESS);
        }
            // A process ID lower than 0 indicates a failure in either process
        else if (pid < 0) {
            exit(EXIT_FAILURE);
        }
        // The parent process has now terminated
        //   and the forked child process will continue
        // (the pid of the child process was 0)

        // Since the child process is a daemon,
        //  the umask needs to be set so files and logs can be written
        umask(0);

        // Open system logs for the child process
        openlog("scheduler", LOG_NOWAIT | LOG_PID, LOG_USER);
        syslog(LOG_NOTICE, "Successfully started scheduler");
	
        // Generate a session ID for the child process
        sid = setsid();
        // Ensure a valid SID for the child process
        if (sid < 0) {
            // Log failure and exit
            syslog(LOG_ERR, "Could not generate session ID for child process");
            // If a new session ID could not be generated,
            //   we must terminate the child process or it will be orphaned
            exit(EXIT_FAILURE);
        }

        // Change the current working directory to a directory guaranteed to exist
        if ((chdir("/")) < 0) {
            // Log failure and exit
            syslog(LOG_ERR, "Could not change working directory to /");
            // If our guaranteed directory does not exist,
            //   terminate the child process to ensure
            // the daemon has not been hijacked
            exit(EXIT_FAILURE);
        }

        // A daemon cannot use the terminal, so close standard
        //   file descriptors for security reasons
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
#endif
    }
    void end(const char *msg)
    {
        // Close system logs for the child process
#ifndef _WIN32
        syslog(LOG_NOTICE, "%s", msg);
        closelog();
#endif
    }
};


#endif //DSERV_DAEMONIZER_HPP
