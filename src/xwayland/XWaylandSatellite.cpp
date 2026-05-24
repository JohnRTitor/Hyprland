#ifdef USE_XWAYLAND_SATELLITE

#include "XWaylandSatellite.hpp"
#include "XWayland.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../debug/log/Logger.hpp"
#include "../helpers/fs/FsUtils.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <format>
#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace Hyprutils::OS;

// Constants
constexpr int SOCKET_DIR_PERMISSIONS = 0755;
constexpr int SOCKET_BACKLOG         = 1;
constexpr int MAX_DISPLAY_RETRIES    = 50;
constexpr int LOCK_FILE_MODE         = 0444;

static bool   safeRemove(const std::string& path) {
    try {
        return std::filesystem::remove(path);
    } catch (const std::exception& e) { Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to remove {}", path); }
    return false;
}

static std::string getSocketPath(int display) {
    return std::format("/tmp/.X11-unix/X{}", display);
}

static CFileDescriptor createListenSocket(struct sockaddr_un* addr, size_t pathSize) {
    const bool        isRegularSocket(addr->sun_path[0]);
    const char        dbgSocketPathPrefix = isRegularSocket ? addr->sun_path[0] : '@';
    const char* const dbgSocketPathRem    = addr->sun_path + 1;

    socklen_t         size = offsetof(struct sockaddr_un, sun_path) + pathSize + 1;
    CFileDescriptor   fd{socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!fd.isValid()) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to create socket {}{}", dbgSocketPathPrefix, dbgSocketPathRem);
        return {};
    }

    if (!fd.setFlags(fd.getFlags() | FD_CLOEXEC)) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to set flags for socket {}{}", dbgSocketPathPrefix, dbgSocketPathRem);
        return {};
    }

    if (isRegularSocket)
        unlink(addr->sun_path);

    if (bind(fd.get(), reinterpret_cast<struct sockaddr*>(addr), size) < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to bind socket {}{}", dbgSocketPathPrefix, dbgSocketPathRem);
        if (isRegularSocket)
            unlink(addr->sun_path);
        return {};
    }

    if (isRegularSocket && chmod(addr->sun_path, 0666) < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to set permission mode for socket {}{}", dbgSocketPathPrefix, dbgSocketPathRem);
    }

    if (listen(fd.get(), SOCKET_BACKLOG) < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to listen to socket {}{}", dbgSocketPathPrefix, dbgSocketPathRem);
        if (isRegularSocket)
            unlink(addr->sun_path);
        return {};
    }

    return fd;
}

CXWaylandSatellite::CXWaylandSatellite() {
    ;
}

CXWaylandSatellite::~CXWaylandSatellite() {
    removeWatches();

    if (m_display < 0)
        return;

    if (!m_lockPath.empty())
        safeRemove(m_lockPath);
    if (!m_socketPathRegular.empty())
        safeRemove(m_socketPathRegular);

    unsetenv("DISPLAY");
}

bool CXWaylandSatellite::ensureSocketDir() {
    if (mkdir("/tmp/.X11-unix", SOCKET_DIR_PERMISSIONS) != 0) {
        if (errno == EEXIST)
            return checkSocketDirPerms();
        else {
            Log::logger->log(Log::ERR, "[XWayland-Satellite] Couldn't create socket dir /tmp/.X11-unix");
            return false;
        }
    }
    return true;
}

bool CXWaylandSatellite::checkSocketDirPerms() {
    struct stat buf;

    if (lstat("/tmp/.X11-unix", &buf)) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to stat X11 socket dir");
        return false;
    }

    if (!(buf.st_mode & S_IFDIR)) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] X11 socket dir is not a directory");
        return false;
    }

    if ((buf.st_uid != 0) && (buf.st_uid != getuid())) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] X11 socket dir is not owned by root or current user");
        return false;
    }

    if (!(buf.st_mode & S_ISVTX)) {
        if ((buf.st_mode & (S_IWGRP | S_IWOTH))) {
            Log::logger->log(Log::ERR, "[XWayland-Satellite] X11 socket dir is writable by others without sticky bit");
            return false;
        }
    }

    return true;
}

bool CXWaylandSatellite::tryOpenSockets() {
    static auto PCREATEABSTRACTSOCKET = CConfigValue<Config::INTEGER>("xwayland:create_abstract_socket");

    if (!ensureSocketDir())
        return false;

    for (int i = 0; i <= MAX_DISPLAY_RETRIES; ++i) {
        std::string     lockPath = std::format("/tmp/.X{}-lock", i);

        CFileDescriptor lockFd{open(lockPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, LOCK_FILE_MODE)};
        if (lockFd.isValid()) {
            // We managed to create the lock file
            sockaddr_un addr = {.sun_family = AF_UNIX};
            std::string path;

            // Socket 0: abstract (if enabled) or regular fallback
#ifdef __linux__
            if (*PCREATEABSTRACTSOCKET) {
                addr.sun_path[0] = '\0';
                path             = getSocketPath(i);
                strncpy(addr.sun_path + 1, path.c_str(), sizeof(addr.sun_path) - 2);
            } else {
                path = std::format("/tmp/.X11-unix/X{}_", i);
                strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
            }
#else
            path = std::format("/tmp/.X11-unix/X{}_", i);
            strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
#endif

            m_xFDs[0] = CFileDescriptor{createListenSocket(&addr, path.length())};

            if (!m_xFDs[0].isValid()) {
                safeRemove(lockPath);
                continue;
            }

            // Socket 1: regular unix socket
            path = getSocketPath(i);
            strncpy(addr.sun_path, path.c_str(), path.length() + 1);
            addr.sun_family = AF_UNIX;

            m_xFDs[1] = CFileDescriptor{createListenSocket(&addr, path.length())};
            if (!m_xFDs[1].isValid()) {
                m_xFDs[0].reset();
                safeRemove(lockPath);
                continue;
            }

            // Write PID to lock file
            const std::string pidStr = std::format("{:010d}\n", getpid());
            if (write(lockFd.get(), pidStr.c_str(), 11) != 11L) {
                m_xFDs[0].reset();
                m_xFDs[1].reset();
                safeRemove(lockPath);
                continue;
            }

            m_display           = i;
            m_displayName       = std::format(":{}", m_display);
            m_lockPath          = lockPath;
            m_socketPathRegular = getSocketPath(i);
            break;
        }

        // Lock file already exists — check if the process is dead
        lockFd = CFileDescriptor{open(lockPath.c_str(), O_RDONLY | O_CLOEXEC)};
        if (!lockFd.isValid())
            continue;

        char pidstr[12] = {0};
        if (read(lockFd.get(), pidstr, sizeof(pidstr) - 1) < 0)
            continue;

        int32_t pid = 0;
        try {
            pid = std::stoi(std::string{pidstr, 11});
        } catch (...) { continue; }

        if (kill(pid, 0) != 0 && errno == ESRCH) {
            if (!safeRemove(lockPath))
                continue;
            i--; // retry this display number
        }
    }

    if (m_display < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to find a suitable display socket");
        return false;
    }

    Log::logger->log(Log::DEBUG, "[XWayland-Satellite] Found suitable display socket at DISPLAY: {}", m_displayName);
    return true;
}

bool CXWaylandSatellite::testOnDemand(const std::string& path) {
    // Test if xwayland-satellite supports on-demand activation
    // We use a pipe and a double-fork because Hyprland sets SIGCHLD to SA_NOCLDWAIT,
    // which causes waitpid to always return ECHILD without the exit status.
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] pipe failed for test: {}", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] fork failed for test: {}", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child 1
        close(pipefd[0]);

        // Clear SA_NOCLDWAIT so waitpid works in this child
        struct sigaction act;
        act.sa_handler = SIG_DFL;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;
        sigaction(SIGCHLD, &act, nullptr);

        pid_t pid2 = fork();
        if (pid2 == 0) {
            // Child 2
            // Redirect stdout/stderr to /dev/null
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            unsetenv("DISPLAY");
            unsetenv("RUST_BACKTRACE");
            unsetenv("RUST_LIB_BACKTRACE");

            execlp(path.c_str(), path.c_str(), ":0", "--test-listenfd-support", nullptr);
            _exit(127); // exec failed
        }

        int status = 0;
        waitpid(pid2, &status, 0);
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
        if (write(pipefd[1], &exit_code, sizeof(exit_code)) < 0) {
            // Nothing we can do
        }
        _exit(0);
    }

    // Parent
    close(pipefd[1]);
    int     exit_code  = -1;
    ssize_t bytes_read = read(pipefd[0], &exit_code, sizeof(exit_code));
    close(pipefd[0]);

    if (bytes_read != sizeof(exit_code)) {
        Log::logger->log(Log::INFO, "[XWayland-Satellite] Error waiting for xwayland-satellite test: Failed to read from pipe");
        return false;
    }

    if (exit_code != 0) {
        if (exit_code == 127)
            Log::logger->log(Log::INFO, "[XWayland-Satellite] xwayland-satellite not found at '{}', disabling integration", path);
        else
            Log::logger->log(Log::INFO, "[XWayland-Satellite] xwayland-satellite doesn't support on-demand activation, disabling integration");
        return false;
    }

    return true;
}

bool CXWaylandSatellite::setup(wl_event_loop* eventLoop) {
    static auto PSATELLITEPATH = CConfigValue<Config::STRING>("xwayland:satellite_path");

    m_eventLoop = eventLoop;

    std::string satPath = *PSATELLITEPATH;
    if (satPath.empty())
        satPath = "xwayland-satellite";

    if (!testOnDemand(satPath))
        return false;

    if (!tryOpenSockets())
        return false;

    setenv("DISPLAY", m_displayName.c_str(), true);
    Log::logger->log(Log::INFO, "[XWayland-Satellite] Listening on X11 socket: {}", m_displayName);

    m_enabled = true;
    setupWatch();
    return true;
}

void CXWaylandSatellite::clearPendingConnections(CFileDescriptor& fd) {
    // Accept and drop all pending connections to prevent busyloop
    // when xwayland-satellite fails to start
    int flags = fcntl(fd.get(), F_GETFL);
    if (flags < 0)
        return;

    fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    socklen_t          len = sizeof(addr);
    while (true) {
        int clientFd = accept(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), &len);
        if (clientFd < 0)
            break;
        close(clientFd);
    }

    fcntl(fd.get(), F_SETFL, flags); // restore
}

void CXWaylandSatellite::removeWatches() {
    if (m_abstractWatch) {
        wl_event_source_remove(m_abstractWatch);
        m_abstractWatch = nullptr;
    }
    if (m_unixWatch) {
        wl_event_source_remove(m_unixWatch);
        m_unixWatch = nullptr;
    }
}

int CXWaylandSatellite::onSocketActivity(int fd, uint32_t mask, void* data) {
    auto* self = static_cast<CXWaylandSatellite*>(data);

    Log::logger->log(Log::DEBUG, "[XWayland-Satellite] Connection on X11 socket; spawning xwayland-satellite");

    // Remove both watchers before spawning
    self->removeWatches();

    // Spawn in a detached thread so we don't block the compositor
    self->spawn();

    return 0;
}

void CXWaylandSatellite::setupWatch() {
    removeWatches();

    if (!m_eventLoop || !m_xFDs[0].isValid() || !m_xFDs[1].isValid())
        return;

    // Clear pending connections before watching
    clearPendingConnections(m_xFDs[0]);
    clearPendingConnections(m_xFDs[1]);

    m_abstractWatch = wl_event_loop_add_fd(m_eventLoop, m_xFDs[0].get(), WL_EVENT_READABLE, onSocketActivity, this);
    m_unixWatch     = wl_event_loop_add_fd(m_eventLoop, m_xFDs[1].get(), WL_EVENT_READABLE, onSocketActivity, this);
}

void CXWaylandSatellite::spawn() {
    static auto PSATELLITEPATH = CConfigValue<Config::STRING>("xwayland:satellite_path");

    std::string satPath = *PSATELLITEPATH;
    if (satPath.empty())
        satPath = "xwayland-satellite";

    // Duplicate FDs to pass to child. The originals must remain open for re-watching.
    CFileDescriptor abstractFd{dup(m_xFDs[0].get())};
    CFileDescriptor unixFd{dup(m_xFDs[1].get())};

    if (!abstractFd.isValid() || !unixFd.isValid()) {
        Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to dup socket fds");
        setupWatch();
        return;
    }

    int abstractRaw = abstractFd.get();
    int unixRaw     = unixFd.get();

    // Spawn in a thread to avoid blocking the compositor event loop
    std::thread([this, satPath, abstractRaw, unixRaw, abstractFd = std::move(abstractFd), unixFd = std::move(unixFd)]() mutable {
        pid_t pid = fork();
        if (pid < 0) {
            Log::logger->log(Log::ERR, "[XWayland-Satellite] fork failed: {}", strerror(errno));
            // Re-register watches from the main thread
            wl_event_loop_add_idle(
                m_eventLoop,
                [](void* data) {
                    auto* self = static_cast<CXWaylandSatellite*>(data);
                    self->setupWatch();
                },
                this);
            return;
        }

        if (pid == 0) {
            // Child process

            // Clear CLOEXEC on the FDs we want to pass
            fcntl(abstractRaw, F_SETFD, 0);
            fcntl(unixRaw, F_SETFD, 0);

            // Redirect output to /dev/null
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            unsetenv("DISPLAY");
            unsetenv("RUST_BACKTRACE");
            unsetenv("RUST_LIB_BACKTRACE");

            std::string abstractStr = std::to_string(abstractRaw);
            std::string unixStr     = std::to_string(unixRaw);

            execlp(satPath.c_str(), satPath.c_str(), m_displayName.c_str(), "-listenfd", abstractStr.c_str(), "-listenfd", unixStr.c_str(), nullptr);

            Log::logger->log(Log::ERR, "[XWayland-Satellite] Failed to exec {}: {}", satPath, strerror(errno));
            _exit(127);
        }

        // Parent (spawner thread)
        // Close our copies, child inherited them
        abstractFd.reset();
        unixFd.reset();

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == ECHILD) {
                // ECHILD is expected because Hyprland sets SA_NOCLDWAIT. The child has terminated.
                Log::logger->log(Log::WARN, "[XWayland-Satellite] xwayland-satellite terminated");
            } else {
                Log::logger->log(Log::WARN, "[XWayland-Satellite] Error waiting for xwayland-satellite: {}", strerror(errno));
            }
        } else {
            Log::logger->log(Log::WARN, "[XWayland-Satellite] xwayland-satellite exited with status {}", WEXITSTATUS(status));
        }

        // Re-register watches from the main thread via idle callback
        wl_event_loop_add_idle(
            m_eventLoop,
            [](void* data) {
                auto* self = static_cast<CXWaylandSatellite*>(data);
                self->setupWatch();
            },
            this);
    }).detach();
}

bool CXWaylandSatellite::enabled() const {
    return m_enabled;
}

const std::string& CXWaylandSatellite::displayName() const {
    return m_displayName;
}

#endif // USE_XWAYLAND_SATELLITE
