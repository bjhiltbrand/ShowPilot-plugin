/**
 * ShowPilot FPP MultiSync Plugin
 *
 * Hooks into FPP's MultiSync system to receive playback position callbacks
 * directly from FPP's engine, and writes them as text lines to a named FIFO
 * that the Node audio daemon reads. Far more precise than polling
 * /api/fppd/status over HTTP.
 *
 * Events written (one per line):
 *   MediaSyncStart/<filename>
 *   MediaSyncStop/<filename>
 *   MediaSyncPacket/<filename>/<seconds>
 *   MediaOpen/<filename>
 */

#include "fpp-pch.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include "Plugin.h"
#include "MultiSync.h"

#define SHOWPILOT_FIFO_PATH "/tmp/SHOWPILOT_FIFO"

class ShowPilotPlugin : public FPPPlugin, public MultiSyncPlugin
{
public:
    ShowPilotPlugin()
        : FPPPlugin("fpp-showpilot-sync")
    {
        LogInfo(VB_PLUGIN, "ShowPilot: Initializing MultiSync plugin\n");
        MultiSync::INSTANCE.addMultiSyncPlugin(this);
        std::lock_guard<std::mutex> lock(m_mutex);
        openFifoLocked();
    }

    virtual ~ShowPilotPlugin()
    {
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fd >= 0) { close(m_fd); m_fd = -1; }
    }

    virtual void SendMediaOpenPacket(const std::string &filename) override
    {
        write("MediaOpen/" + filename + "\n");
    }

    virtual void SendMediaSyncStartPacket(const std::string &filename) override
    {
        m_lastMediaHalfSecond = -1;
        write("MediaSyncStart/" + filename + "\n");
        LogInfo(VB_PLUGIN, "ShowPilot: MediaSyncStart: %s\n", filename.c_str());
    }

    virtual void SendMediaSyncStopPacket(const std::string &filename) override
    {
        m_lastMediaHalfSecond = -1;
        write("MediaSyncStop/" + filename + "\n");
        LogInfo(VB_PLUGIN, "ShowPilot: MediaSyncStop: %s\n", filename.c_str());
    }

    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) override
    {
        // Only send when the half-second boundary changes — ~2 updates/sec is enough.
        int curTS = static_cast<int>(seconds * 2.0f);
        if (m_lastMediaHalfSecond.exchange(curTS) == curTS) return;

        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", (double)seconds);
        write("MediaSyncPacket/" + filename + "/" + std::string(buf) + "\n");
    }

private:
    int m_fd = -1;
    std::atomic<int> m_lastMediaHalfSecond{-1};
    std::mutex m_mutex;
    bool m_warnedNotFifo = false;

    // fppd runs as root and /tmp is world-writable, so never follow a
    // symlink or reuse a non-FIFO someone else planted at this path.
    // Caller holds m_mutex.
    void openFifoLocked()
    {
        struct stat st;
        if (lstat(SHOWPILOT_FIFO_PATH, &st) != 0) {
            if (mkfifo(SHOWPILOT_FIFO_PATH, 0660) != 0 && errno != EEXIST) {
                LogWarn(VB_PLUGIN, "ShowPilot: mkfifo failed: %s\n", strerror(errno));
                return;
            }
            // The audio daemon runs as fpp, not root.
            if (struct passwd *pw = getpwnam("fpp")) {
                if (lchown(SHOWPILOT_FIFO_PATH, pw->pw_uid, pw->pw_gid) != 0) {
                    LogWarn(VB_PLUGIN, "ShowPilot: chown of FIFO failed: %s\n", strerror(errno));
                }
            }
        } else if (!S_ISFIFO(st.st_mode)) {
            if (!m_warnedNotFifo) {
                LogWarn(VB_PLUGIN, "ShowPilot: %s exists and is not a FIFO; ignoring it\n", SHOWPILOT_FIFO_PATH);
                m_warnedNotFifo = true;
            }
            return;
        }

        // Non-blocking: fails with ENXIO until the daemon has its end open,
        // and never blocks fppd if the daemon stops reading.
        m_fd = open(SHOWPILOT_FIFO_PATH, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (m_fd < 0) {
            LogDebug(VB_PLUGIN, "ShowPilot: FIFO not ready (daemon not running): %s\n", strerror(errno));
            return;
        }
        if (fstat(m_fd, &st) != 0 || !S_ISFIFO(st.st_mode)) {
            close(m_fd);
            m_fd = -1;
            return;
        }
        fchmod(m_fd, 0660);
        LogInfo(VB_PLUGIN, "ShowPilot: FIFO opened: %s\n", SHOWPILOT_FIFO_PATH);
    }

    // Called from fppd's media threads; m_fd is shared, so serialize.
    void write(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fd < 0) {
            // The daemon may have started since the last attempt.
            openFifoLocked();
            if (m_fd < 0) return;
        }

        ssize_t ret = ::write(m_fd, message.c_str(), message.size());
        if (ret < 0 && (errno == EPIPE || errno == ENXIO)) {
            // Daemon closed its end — reopen on the next message.
            close(m_fd);
            m_fd = -1;
        }
        // EAGAIN means the pipe is full: drop the message rather than block.
    }
};

extern "C" {
    FPPPlugin *createPlugin() {
        return new ShowPilotPlugin();
    }
}
