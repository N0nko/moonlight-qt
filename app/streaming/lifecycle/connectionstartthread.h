#pragma once

#include <QThread>

class Session;

class AsyncConnectionStartThread final : public QThread
{
public:
    explicit AsyncConnectionStartThread(Session* session,
                                        bool recoveryAttempt = false,
                                        bool fastResume = false);

protected:
    void run() override;

private:
    Session* m_Session;
    bool m_RecoveryAttempt;
    bool m_FastResume;
};
