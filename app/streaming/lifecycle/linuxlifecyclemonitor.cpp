#include "linuxlifecyclemonitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QVariant>

#include <chrono>
#include <utility>

namespace {
constexpr auto kSleepQuiesceTimeout = std::chrono::milliseconds(1500);
constexpr unsigned int kNetworkManagerConnectedLocal = 50;
}

class LinuxLifecycleReceiver : public QObject
{
    Q_OBJECT

public:
    explicit LinuxLifecycleReceiver(LinuxLifecycleMonitor* monitor)
        : m_Monitor(monitor),
          m_LogindSubscribed(false),
          m_NetworkSubscribed(false)
    {
    }

    bool startMonitoring()
    {
        QDBusConnection connection = QDBusConnection::systemBus();
        if (!connection.isConnected()) {
            qWarning() << "Unable to connect to the system bus for lifecycle monitoring";
            return false;
        }

        m_LogindSubscribed = connection.connect(
                    QStringLiteral("org.freedesktop.login1"),
                    QStringLiteral("/org/freedesktop/login1"),
                    QStringLiteral("org.freedesktop.login1.Manager"),
                    QStringLiteral("PrepareForSleep"),
                    this,
                    SLOT(prepareForSleep(bool)));
        if (m_LogindSubscribed) {
            acquireInhibitor();
        }
        else {
            qWarning() << "Unable to subscribe to logind PrepareForSleep";
        }

        m_NetworkSubscribed = connection.connect(
                    QStringLiteral("org.freedesktop.NetworkManager"),
                    QStringLiteral("/org/freedesktop/NetworkManager"),
                    QStringLiteral("org.freedesktop.NetworkManager"),
                    QStringLiteral("StateChanged"),
                    this,
                    SLOT(networkStateChanged(uint)));
        if (m_NetworkSubscribed) {
            refreshNetworkState();
        }
        else {
            qInfo() << "NetworkManager lifecycle signals are unavailable";
        }

        return m_LogindSubscribed || m_NetworkSubscribed;
    }

    void stopMonitoring()
    {
        QDBusConnection connection = QDBusConnection::systemBus();
        if (m_LogindSubscribed) {
            connection.disconnect(
                        QStringLiteral("org.freedesktop.login1"),
                        QStringLiteral("/org/freedesktop/login1"),
                        QStringLiteral("org.freedesktop.login1.Manager"),
                        QStringLiteral("PrepareForSleep"),
                        this,
                        SLOT(prepareForSleep(bool)));
        }
        if (m_NetworkSubscribed) {
            connection.disconnect(
                        QStringLiteral("org.freedesktop.NetworkManager"),
                        QStringLiteral("/org/freedesktop/NetworkManager"),
                        QStringLiteral("org.freedesktop.NetworkManager"),
                        QStringLiteral("StateChanged"),
                        this,
                        SLOT(networkStateChanged(uint)));
        }
        releaseInhibitor();
    }

private slots:
    void prepareForSleep(bool sleeping)
    {
        if (sleeping) {
            if (m_NetworkSubscribed) {
                // Require a fresh usable NetworkManager state after wake.
                m_Monitor->notifyNetworkState(0);
            }
            m_Monitor->notifySleepState(true);
            m_Monitor->waitForSleepReady();
            releaseInhibitor();
        }
        else {
            // NetworkManager can still be disconnected at wake. Publish that
            // state before the wake event so Moonlight waits rather than churns.
            refreshNetworkState();
            m_Monitor->notifySleepState(false);
            acquireInhibitor();
        }
    }

    void networkStateChanged(uint state)
    {
        m_Monitor->notifyNetworkState(state);
    }

private:
    void refreshNetworkState()
    {
        QDBusInterface manager(QStringLiteral("org.freedesktop.NetworkManager"),
                               QStringLiteral("/org/freedesktop/NetworkManager"),
                               QStringLiteral("org.freedesktop.NetworkManager"),
                               QDBusConnection::systemBus());
        if (!manager.isValid()) {
            return;
        }

        const QVariant state = manager.property("State");
        if (state.isValid()) {
            m_Monitor->notifyNetworkState(state.toUInt());
        }
    }

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

    LinuxLifecycleMonitor* m_Monitor;
    bool m_LogindSubscribed;
    bool m_NetworkSubscribed;
    QDBusUnixFileDescriptor m_Inhibitor;
};

LinuxLifecycleMonitor::LinuxLifecycleMonitor(
        std::function<void(bool)> sleepCallback,
        std::function<void(bool)> networkCallback)
    : m_SleepCallback(std::move(sleepCallback)),
      m_NetworkCallback(std::move(networkCallback)),
      m_SleepReady(false),
      m_Stopping(false)
{
}

LinuxLifecycleMonitor::~LinuxLifecycleMonitor()
{
    stopMonitoring();
}

void LinuxLifecycleMonitor::acknowledgeSleepReady()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_SleepReady = true;
    }
    m_Condition.notify_all();
}

void LinuxLifecycleMonitor::stopMonitoring()
{
    if (m_Stopping.exchange(true)) {
        wait();
        return;
    }

    m_Condition.notify_all();
    quit();
    wait();
}

void LinuxLifecycleMonitor::run()
{
    LinuxLifecycleReceiver receiver(this);
    if (!receiver.startMonitoring()) {
        return;
    }

    exec();
    receiver.stopMonitoring();
}

void LinuxLifecycleMonitor::notifySleepState(bool sleeping)
{
    if (m_Stopping.load()) {
        return;
    }

    if (sleeping) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_SleepReady = false;
    }

    m_SleepCallback(sleeping);
}

void LinuxLifecycleMonitor::notifyNetworkState(unsigned int state)
{
    if (!m_Stopping.load()) {
        m_NetworkCallback(state >= kNetworkManagerConnectedLocal);
    }
}

void LinuxLifecycleMonitor::waitForSleepReady()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Condition.wait_for(lock, kSleepQuiesceTimeout, [this]() {
        return m_SleepReady || m_Stopping.load();
    });
}

#include "linuxlifecyclemonitor.moc"
