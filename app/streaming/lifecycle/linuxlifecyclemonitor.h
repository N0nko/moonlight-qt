#pragma once

#include <QThread>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

class LinuxLifecycleMonitor : public QThread
{
public:
    LinuxLifecycleMonitor(std::function<void(bool)> sleepCallback,
                          std::function<void(bool)> networkCallback);
    ~LinuxLifecycleMonitor() override;

    void acknowledgeSleepReady();
    void stopMonitoring();

protected:
    void run() override;

private:
    friend class LinuxLifecycleReceiver;

    void notifySleepState(bool sleeping);
    void notifyNetworkState(unsigned int state);
    void waitForSleepReady();

    std::function<void(bool)> m_SleepCallback;
    std::function<void(bool)> m_NetworkCallback;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    bool m_SleepReady;
    std::atomic_bool m_Stopping;
};
