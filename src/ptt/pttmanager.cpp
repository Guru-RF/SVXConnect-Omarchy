/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ptt/pttmanager.h"
#include "ptt/portalbackend.h"

PttManager::PttManager(svx_app *app, QObject *parent)
    : QObject(parent), m_app(app)
{
    auto *portal = new PortalBackend(this);
    m_backends.append(portal);
    wire(portal);
}

PttManager::~PttManager()
{
    /* app_free() would unkey on the way out, but the reflector would see a
     * talker vanish without a flush and the operator would hear their own
     * audio stop mid-word. Do it explicitly, first. */
    forceUnkey("shutting down");
    for (PttBackend *b : std::as_const(m_backends))
        b->stop();
}

PttBackend *PttManager::backend(const QString &id) const
{
    for (PttBackend *b : m_backends)
        if (b->id() == id)
            return b;
    return nullptr;
}

void PttManager::wire(PttBackend *b)
{
    connect(b, &PttBackend::pressed,  this, [this, b]() { onPressed(b); });
    connect(b, &PttBackend::released, this, [this, b]() { onReleased(b); });

    connect(b, &PttBackend::lost, this, [this, b](const QString &why) {
        /* Defence 2. The backend has told us it can no longer see the release,
         * so anything it is currently holding is now unbounded. */
        log_warn("ptt: %s lost: %s", qPrintable(b->id()), qPrintable(why));
        forceUnkey("the push-to-talk control was lost");
        emit backendLost(b->id(), why);
    });

    connect(b, &PttBackend::triggerChanged, this, [this, b](const QString &human) {
        emit triggerChanged(b->id(), human);
    });
}

void PttManager::applyKeyboardBinding(const PttBinding &binding)
{
    PttBackend *portal = backend(QStringLiteral("portal"));
    if (!portal)
        return;

    if (!binding.isValid()) {
        portal->stop();
        return;
    }

    /* Do not tear down a working session for nothing.
     *
     * The dialog re-applies bindings whenever any push-to-talk setting is
     * touched, but the keyboard trigger itself is owned by the desktop and
     * cannot be edited here — so in practice it is almost always unchanged.
     * Restarting the portal session in that case is pure risk: it drops the
     * grab, recreates the session, and briefly leaves no shortcut at all. */
    if (portal->isActive()) {
        log_dbg("ptt: keyboard binding unchanged (%s), leaving it alone",
                qPrintable(portal->activeTrigger()));
        return;
    }

    if (!portal->start(binding))
        log_warn("ptt: the keyboard binding could not be started");
}

void PttManager::onPressed(PttBackend *b)
{
    Q_UNUSED(b);
    if (!m_app)
        return;

    if (m_mode == Mode::Toggle) {
        /* One edge only. The release is ignored, so a toggle behaves the same
         * whether the backend reports releases or not — which is what makes
         * toggle the honest fallback on a compositor that cannot. */
        m_latched = !m_latched;
        app_ptt(m_app, m_latched ? CTL_ON : CTL_OFF);
        return;
    }

    if (++m_holders == 1)
        app_ptt(m_app, CTL_ON);
}

void PttManager::onReleased(PttBackend *b)
{
    Q_UNUSED(b);
    if (!m_app || m_mode == Mode::Toggle)
        return;

    if (m_holders > 0 && --m_holders == 0)
        app_ptt(m_app, CTL_OFF);
}

void PttManager::forceUnkey(const char *why)
{
    const bool wasKeyed = isKeyed();

    m_holders = 0;
    m_latched = false;

    if (!m_app)
        return;

    /* Unconditionally, not only when we think we were keyed: the whole point
     * of this path is that our idea of the state may be wrong. app_ptt(OFF)
     * on an already-idle transmitter is a no-op in the core. */
    app_ptt(m_app, CTL_OFF);

    if (wasKeyed)
        log_info("ptt: un-keyed (%s)", why);
}
