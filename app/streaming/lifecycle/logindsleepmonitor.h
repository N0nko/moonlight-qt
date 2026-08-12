#pragma once

#include <QThread>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

class LogindSleepMonitor : public QThread
{
public:
    explicit LogindSleepMonitor(std::function<void(bool)> callback);
    ~LogindSleepMonitor() override;

    void acknowledgeSleepReady();
    void stopMonitoring();

protected:
    void run() override;

private:
    friend class LogindSleepReceiver;

    void notifySleepState(bool sleeping);
    void waitForSleepReady();

    std::function<void(bool)> m_Callback;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    bool m_SleepReady;
    std::atomic_bool m_Stopping;
};
