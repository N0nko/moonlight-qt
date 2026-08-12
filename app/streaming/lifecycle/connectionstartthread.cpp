#include "connectionstartthread.h"

#include "streaming/session.h"

AsyncConnectionStartThread::AsyncConnectionStartThread(Session* session,
                                                       bool recoveryAttempt,
                                                       bool fastResume)
    : QThread(nullptr),
      m_Session(session),
      m_RecoveryAttempt(recoveryAttempt),
      m_FastResume(fastResume)
{
    setObjectName(recoveryAttempt ? "Recovery Conn Start" : "Async Conn Start");
}

void AsyncConnectionStartThread::run()
{
    m_Session->m_AsyncConnectionSuccess.store(
                m_Session->startConnectionAsync(m_RecoveryAttempt,
                                                m_FastResume));
}
