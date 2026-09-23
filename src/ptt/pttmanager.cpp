/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ptt/pttmanager.h"
#include "ptt/portalbackend.h"
#include "core/coreaction.h"

PttManager::PttManager(svx_app *app, QObject *parent)
    : PttManager(app, {new PortalBackend}, parent)
{
}

PttManager::PttManager(svx_app *app, const QVector<PttBackend *> &backends, QObject *parent)
    : QObject(parent), m_app(app)
{
    for (PttBackend *b : backends) {
        b->setParent(this);
        m_backends.append(b);
        wire(b);
    }
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

void PttManager::setMode(Mode m)
{
    if (m == m_mode)
        return;

    /* Hold -> Toggle with the key down would ignore the release it is waiting
     * for; Toggle -> Hold would wait for a release that is never coming. */
    if (m_holders > 0 || isKeyed())
        forceUnkey("the push-to-talk mode changed");
    m_mode = m;
}

void PttManager::applyKeyboardBinding(const PttBinding &binding)
{
    PttBackend *portal = backend(QStringLiteral("portal"));
    if (!portal)
        return;

    if (!binding.isValid()) {
        /* stop() forgets the session, so a Deactivated still in flight for it
         * is ignored — un-key now rather than wait for a release that will not
         * be delivered. */
        if (m_holders > 0 || isKeyed())
            forceUnkey("the keyboard binding was removed");
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

    /* start() closes whatever session there was first; same reasoning. */
    if (m_holders > 0 || isKeyed())
        forceUnkey("the keyboard binding was restarted");
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
         * toggle the honest fallback on a compositor that cannot.
         *
         * CTL_TOGGLE, which the core resolves against its own tx_active: the
         * only state that is never out of date. See m_holders in the header
         * for what a private latch did instead. */
        CoreAction::ptt(m_app, CTL_TOGGLE);
        return;
    }

    if (++m_holders == 1)
        CoreAction::ptt(m_app, CTL_ON);
}

void PttManager::onReleased(PttBackend *b)
{
    Q_UNUSED(b);
    if (!m_app || m_mode == Mode::Toggle)
        return;

    if (m_holders > 0 && --m_holders == 0)
        CoreAction::ptt(m_app, CTL_OFF);
}

void PttManager::forceUnkey(const char *why)
{
    const bool wasKeyed = isKeyed();

    m_holders = 0;

    if (!m_app)
        return;

    /* Unconditionally, not only when we think we were keyed: the whole point
     * of this path is that our idea of the state may be wrong. app_ptt(OFF)
     * on an already-idle transmitter is a no-op in the core. */
    CoreAction::ptt(m_app, CTL_OFF);

    if (wasKeyed)
        log_info("ptt: un-keyed (%s)", why);
}
