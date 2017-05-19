#include <string>
#include <iostream>
#include <functional>
#include <system_error>
#include <vector>

#include <cstring>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <libcgroup.h>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <syscall.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "sandbox.h"
#include "utils.h"
#include "cgroup.h"
#include "semaphore.h"
#include "pipe.h"

namespace fs = boost::filesystem;
using std::string;
using std::vector;
using boost::format;

// Make sure fd 0,1,2 exists.
static void RedirectIO(const string &std_input, const string &std_output,
                       const string &std_error, int nullfd)
{
    int inputfd, outputfd, errorfd;
    if (std_input != "")
    {
        inputfd = Ensure(open(std_input.c_str(), O_RDONLY));
    }
    else
    {
        inputfd = nullfd;
    }
    Ensure(dup2(inputfd, STDIN_FILENO));

    if (std_output != "")
    {
        outputfd = Ensure(open(std_output.c_str(), O_WRONLY | O_TRUNC | O_CREAT,
                               S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP));
    }
    else
    {
        outputfd = nullfd;
    }
    Ensure(dup2(outputfd, STDOUT_FILENO));

    if (std_error != "")
    {
        if (std_error == std_output)
        {
            errorfd = outputfd;
        }
        else
        {
            errorfd = Ensure(open(std_error.c_str(), O_WRONLY | O_TRUNC | O_CREAT,
                                  S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP));
        }
    }
    else
    {
        errorfd = nullfd;
    }
    Ensure(dup2(errorfd, STDERR_FILENO));
}

struct ExecutionParameter
{
    const SandboxParameter &parameter;

    PosixSemaphore semaphore1, semaphore2;
    // This pipe is used to forward error message from the child process to the parent.
    PosixPipe pipefd;

    ExecutionParameter(const SandboxParameter &param, int pipeOptions) : parameter(param),
                                                                         semaphore1(true, 0),
                                                                         semaphore2(true, 0),
                                                                         pipefd(pipeOptions)
    {
    }
};

static int ChildProcess(void *param_ptr)
{
    ExecutionParameter &execParam = *reinterpret_cast<ExecutionParameter *>(param_ptr);
    // We obtain a full copy of parameters here. The arguments may be destoryed after some time.
    SandboxParameter parameter = execParam.parameter;

    try
    {
        Ensure(close(execParam.pipefd[0]));
        passwd *newUser = nullptr;
        if (parameter.userName != "")
        {
            // Get the user info before chroot, or it will be unable to open /etc/passwd
            newUser = CheckNull(getpwnam(parameter.userName.c_str()));
        }

        int nullfd = Ensure(open("/dev/null", O_RDWR));
        if (parameter.redirectBeforeChroot)
        {
            RedirectIO(parameter.stdinRedirection, parameter.stdoutRedirection,
                       parameter.stderrRedirection, nullfd);
        }

        // TODO: choose a better place for the temp path.
        fs::path tempRoot("/tmp");
        Ensure(mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL)); // Make root private
        Ensure(mount(parameter.chrootDirectory.string().c_str(), tempRoot.string().c_str(), "", MS_BIND | MS_REC, ""));
        Ensure(mount("", tempRoot.string().c_str(), "", MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, ""));

        for (MountInfo &info : parameter.mounts)
        {
            fs::path target = tempRoot / info.dst;
            std::cerr << "Binding " << info.src << " to " << target << std::endl;
            Ensure(mount(info.src.string().c_str(), target.string().c_str(), "", MS_BIND | MS_REC, ""));
            if (info.limit == 0)
            {
                Ensure(mount("", target.string().c_str(), "", MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, ""));
            }
            else if (info.limit != -1)
            {
                // TODO: implement.
            }
        }

        Ensure(chroot(tempRoot.string().c_str()));
        std::cerr << "Working: " << parameter.workingDirectory << std::endl;
        Ensure(chdir(parameter.workingDirectory.string().c_str()));

        if (parameter.mountProc)
        {
            Ensure(mount("proc", "/proc", "proc", 0, NULL));
        }
        if (!parameter.redirectBeforeChroot)
        {
            RedirectIO(parameter.stdinRedirection, parameter.stdoutRedirection,
                       parameter.stderrRedirection, nullfd);
        }

        const char *newHostname = "BraveNewWorld";
        Ensure(sethostname(newHostname, strlen(newHostname)));

        if (newUser != nullptr)
        {
            Ensure(setgid(newUser->pw_gid));
            Ensure(setuid(newUser->pw_uid));
        }

        vector<char *> params = StringToPtr(parameter.executableParameters),
                       envi = StringToPtr(parameter.environmentVariables);

        int temp = -1;
        // Inform the parent that no exception occurred.
        Ensure(write(execParam.pipefd[1], &temp, sizeof(int)));

        // Inform our parent that we are ready to go.
        execParam.semaphore1.Post();
        // Wait for parent's reply.
        execParam.semaphore2.Wait();

std:: cerr << parameter.executablePath << std::endl;
        Ensure(execve(parameter.executablePath.c_str(), &params[0], &envi[0]));

        // If execve returns, then we meet an error.
        raise(SIGABRT);
        return 255;
    }
    catch (std::exception &err)
    {
        // TODO: implement error handling
        // abort(); // This will cause segmentation fault.
        // throw;
        // return 222;

        const char *errMessage = err.what();
        int len = strlen(errMessage);
        try
        {
            Ensure(write(execParam.pipefd[1], &len, sizeof(int)));
            Ensure(write(execParam.pipefd[1], errMessage, len));
            Ensure(close(execParam.pipefd[1]));
            std::cerr << errMessage << std::endl;
            execParam.semaphore1.Post();
            return 126;
        }
        catch (...)
        {
            return 125;
        }
    }
    catch (...)
    {
        return 125;
    }
}

// The child stack is only used before `execve`, so it does not need much space.
const int childStackSize = 1024 * 700;
pid_t StartSandbox(const SandboxParameter &parameter
                   /* ,std::function<void(pid_t)> reportPid*/) // Let's use some fancy C++11 feature.
{
    pid_t container_pid = -1;
    try
    {
        // char* childStack = new char[childStackSize];
        std::vector<char> childStack(childStackSize); // I don't want to call `delete`

        ExecutionParameter execParam(parameter, O_CLOEXEC | O_NONBLOCK);

        container_pid = Ensure(clone(ChildProcess, &*childStack.end(),
                                     CLONE_NEWNET | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                                     const_cast<void *>(reinterpret_cast<const void *>(&execParam))));

        CgroupInfo memInfo("memory", parameter.cgroupName),
            cpuInfo("cpuacct", parameter.cgroupName),
            pidInfo("pids", parameter.cgroupName);

        vector<CgroupInfo *> infos = {&memInfo, &cpuInfo, &pidInfo};
        for (auto &item : infos)
        {
            CreateGroup(*item);
            KillGroupMembers(*item);
            WriteGroupProperty(*item, "tasks", container_pid);
        }

#define WRITE_WITH_CHECK(__where, __name, __value)                  \
    {                                                               \
        if ((__value) >= 0)                                         \
        {                                                           \
            WriteGroupProperty((__where), (__name), (__value));     \
        }                                                           \
        else                                                        \
        {                                                           \
            WriteGroupProperty((__where), (__name), string("max")); \
        }                                                           \
    }

        // Forcibly clear any memory usage by cache.
        WriteGroupProperty(memInfo, "memory.force_empty", 0);
        WRITE_WITH_CHECK(memInfo, "memory.limit_in_bytes", parameter.memoryLimit);
        WRITE_WITH_CHECK(memInfo, "memory.memsw.limit_in_bytes", parameter.memoryLimit);
        WRITE_WITH_CHECK(pidInfo, "pids.max", parameter.processLimit);

        // Wait for at most 100ms. If the child process hasn't posted the semaphore,
        // We will assume that the child has already dead.
        bool waitResult = execParam.semaphore1.TimedWait(0, 100 * 1000 * 1000);

        int errLen, bytesRead = read(execParam.pipefd[0], &errLen, sizeof(int));
        // Child will be killed once the error has been thrown.
        if (!waitResult || bytesRead == 0 || bytesRead == -1)
        {
            // No information available.
            throw std::runtime_error("The child process has exited unexpectedly.");
        }
        else if (errLen != -1) // -1 indicates OK.
        {
            vector<char> buf(errLen);
            Ensure(read(execParam.pipefd[0], &*buf.begin(), errLen));
            string errstr(buf.begin(), buf.end());
            throw std::runtime_error((format("The child process has reported the following error: %1%") % errstr).str());
        }

        // Clear usage stats.
        WriteGroupProperty(memInfo, "memory.memsw.max_usage_in_bytes", 0);
        WriteGroupProperty(cpuInfo, "cpuacct.usage", 0);

        // Continue the child.
        execParam.semaphore2.Post();

        // Wait for 1ms to prevent the child stack deallocated before execve.
        // TODO: Find a better way to handle this.
        usleep(1000);

        return container_pid;
    }
    catch (std::exception &ex)
    {
        // Do the cleanups; we don't care whether these operations are successful.
        if (container_pid != -1)
        {
            (void)kill(container_pid, SIGKILL);
            (void)waitpid(container_pid, NULL, WNOHANG);
        }
        throw;
    }
}

ExecutionResult
SBWaitForProcess(pid_t pid)
{
    ExecutionResult result;
    int status;
    Ensure(waitpid(pid, &status, 0));
    if (WIFEXITED(status))
    {
        result.Status = EXITED;
        result.Code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        result.Status = SIGNALED;
        result.Code = WTERMSIG(status);
    }
    return result;
}