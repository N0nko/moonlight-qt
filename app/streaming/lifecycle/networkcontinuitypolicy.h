#pragma once

enum class NetworkContinuityAction {
    None,
    RetainTransport,
    ReleaseRecoveryGate,
};

constexpr NetworkContinuityAction networkContinuityAction(bool available,
                                                           bool unavailable)
{
    if (available == !unavailable) {
        return NetworkContinuityAction::None;
    }

    return available ? NetworkContinuityAction::ReleaseRecoveryGate
                     : NetworkContinuityAction::RetainTransport;
}
