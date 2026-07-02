#include <linux/capability.h>
#include <sys/prctl.h>
#include <gio/gio.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "initHelpers.hpp"

bool NInit::isSudo() {
    return getuid() != geteuid() || !geteuid();
}

static bool tryRtkit() {
    struct rlimit rl;
    rl.rlim_cur = 200000;
    rl.rlim_max = 200000;
    if (setrlimit(RLIMIT_RTTIME, &rl) != 0) {
        Log::logger->log(Log::WARN, "Failed to set RLIMIT_RTTIME");
        return false;
    }

    pid_t            tid = syscall(SYS_gettid);

    GError*          error = nullptr;
    GDBusConnection* conn  = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!conn) {
        if (error) {
            Log::logger->log(Log::WARN, "Failed to connect to system D-Bus: {}", error->message);
            g_error_free(error);
        }
        return false;
    }

    const int minPrio = sched_get_priority_min(SCHED_RR);

    GVariant* result = g_dbus_connection_call_sync(conn, "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "MakeThreadRealtime",
                                                   g_variant_new("(tu)", (guint64)tid, (guint32)minPrio), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    g_object_unref(conn);

    if (error) {
        Log::logger->log(Log::WARN, "rtkit request failed: {}", error->message);
        g_error_free(error);
        return false;
    }

    if (result)
        g_variant_unref(result);

    return true;
}

static bool tryCapSysNice() {
    const int          minPrio = sched_get_priority_min(SCHED_RR);
    int                old_policy;
    struct sched_param param;

    if (pthread_getschedparam(pthread_self(), &old_policy, &param)) {
        Log::logger->log(Log::WARN, "Failed to get old pthread scheduling priority");
        return false;
    }

    param.sched_priority = minPrio;

    if (pthread_setschedparam(pthread_self(), SCHED_RR, &param)) {
        Log::logger->log(Log::WARN, "Failed to change process scheduling strategy");
        return false;
    }

    return true;
}

void NInit::gainRealTime() {
    Log::logger->log(Log::INFO, "Attempting to acquire real-time priority via rtkit...");
    if (tryRtkit())
        Log::logger->log(Log::INFO, "Real-time priority granted by rtkit.");
    else {
        Log::logger->log(Log::WARN, "rtkit unavailable or request denied. Falling back to CAP_SYS_NICE.");
        if (tryCapSysNice())
            Log::logger->log(Log::INFO, "Real-time priority acquired using CAP_SYS_NICE.");
        else
            Log::logger->log(Log::WARN, "Unable to obtain real-time priority (rtkit unavailable and CAP_SYS_NICE missing).");
    }

    // NixOS-specific fix to prevent all children from inheriting
    // CAP_SYS_NICE due to how the security wrapper works.
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_SYS_NICE, 0, 0);

    pthread_atfork(nullptr, nullptr, []() {
        const struct sched_param param = {.sched_priority = 0};
        if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param))
            Log::logger->log(Log::WARN, "Failed to reset process scheduling strategy");
    });
}