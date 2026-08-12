#include "logindsleepmonitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDebug>

#include <chrono>
#include <utility>

namespace {
constexpr auto kSleepQuiesceTimeout = std::chrono::milliseconds(1500);
}

class LogindSleepReceiver : public QObject
{
    Q_OBJECT

public:
    explicit LogindSleepReceiver(LogindSleepMonitor* monitor)
        : m_Monitor(monitor)
    {
    }

    bool startMonitoring()
    {
        QDBusConnection connection = QDBusConnection::systemBus();
        if (!connection.isConnected()) {
            qWarning() << "Unable to connect to logind for sleep monitoring";
            return false;
        }

        if (!connection.connect(QStringLiteral("org.freedesktop.login1"),
                                QStringLiteral("/org/freedesktop/login1"),
                                QStringLiteral("org.freedesktop.login1.Manager"),
                                QStringLiteral("PrepareForSleep"),
                                this,
                                SLOT(prepareForSleep(bool)))) {
            qWarning() << "Unable to subscribe to logind PrepareForSleep";
            return false;
        }

        acquireInhibitor();
        return true;
    }

    void stopMonitoring()
    {
        QDBusConnection::systemBus().disconnect(
                    QStringLiteral("org.freedesktop.login1"),
                    QStringLiteral("/org/freedesktop/login1"),
                    QStringLiteral("org.freedesktop.login1.Manager"),
                    QStringLiteral("PrepareForSleep"),
                    this,
                    SLOT(prepareForSleep(bool)));
        releaseInhibitor();
    }

private slots:
    void prepareForSleep(bool sleeping)
    {
        if (sleeping) {
            m_Monitor->notifySleepState(true);
            m_Monitor->waitForSleepReady();
            releaseInhibitor();
        }
        else {
            m_Monitor->notifySleepState(false);
            acquireInhibitor();
        }
    }

private:
    void acquireInhibitor()
    {
        QDBusInterface manager(QStringLiteral("org.freedesktop.login1"),
                               QStringLiteral("/org/freedesktop/login1"),
                               QStringLiteral("org.freedesktop.login1.Manager"),
                               QDBusConnection::systemBus());
        manager.setTimeout(1000);

        QDBusPendingReply<QDBusUnixFileDescriptor> reply = manager.asyncCall(
                    QStringLiteral("Inhibit"),
                    QStringLiteral("sleep"),
                    QStringLiteral("Moonlight"),
                    QStringLiteral("Quiesce the active stream"),
                    QStringLiteral("delay"));
        reply.waitForFinished();
        if (reply.isError()) {
            qWarning() << "Unable to acquire logind sleep inhibitor:"
                       << reply.error().message();
            return;
        }

        m_Inhibitor = reply.value();
    }

    void releaseInhibitor()
    {
        m_Inhibitor = QDBusUnixFileDescriptor();
    }

    LogindSleepMonitor* m_Monitor;
    QDBusUnixFileDescriptor m_Inhibitor;
};

LogindSleepMonitor::LogindSleepMonitor(std::function<void(bool)> callback)
    : m_Callback(std::move(callback)),
      m_SleepReady(false),
      m_Stopping(false)
{
}

LogindSleepMonitor::~LogindSleepMonitor()
{
    stopMonitoring();
}

void LogindSleepMonitor::acknowledgeSleepReady()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_SleepReady = true;
    }
    m_Condition.notify_all();
}

void LogindSleepMonitor::stopMonitoring()
{
    if (m_Stopping.exchange(true)) {
        wait();
        return;
    }

    m_Condition.notify_all();
    quit();
    wait();
}

void LogindSleepMonitor::run()
{
    LogindSleepReceiver receiver(this);
    if (!receiver.startMonitoring()) {
        return;
    }

    exec();
    receiver.stopMonitoring();
}

void LogindSleepMonitor::notifySleepState(bool sleeping)
{
    if (m_Stopping.load()) {
        return;
    }

    if (sleeping) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_SleepReady = false;
    }

    m_Callback(sleeping);
}

void LogindSleepMonitor::waitForSleepReady()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Condition.wait_for(lock, kSleepQuiesceTimeout, [this]() {
        return m_SleepReady || m_Stopping.load();
    });
}

#include "logindsleepmonitor.moc"
