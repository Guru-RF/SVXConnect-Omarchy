/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * See runlock.h for why the owner kind is "gui-omarchy" and not "gui".
 */
#include "core/runlock.h"
#include "core/svxcore.h"

#include <QCoreApplication>

namespace RunLock {

QString Holder::describe() const
{
    /* These strings are user-facing and end up in a dialog and in the tray
     * tooltip. Name the actual program, not the internal kind token: "gui"
     * means nothing to someone who installed a package called svxconnect-pios.
     */
    QString who;
    if (kind == QLatin1String("cli"))
        who = QCoreApplication::translate("RunLock", "the terminal client (svxconnect)");
    else if (kind == QLatin1String("headless"))
        who = QCoreApplication::translate("RunLock", "the background service (svxconnect --headless)");
    else if (isPios())
        who = QCoreApplication::translate("RunLock", "SVXConnect for Raspberry Pi OS");
    else if (kind == QLatin1String("gui-qt"))
        who = QCoreApplication::translate("RunLock", "SVXConnect-Qt");
    else if (isSelfKind())
        who = QCoreApplication::translate("RunLock", "another SVXConnect window");
    else if (kind.isEmpty())
        who = QCoreApplication::translate("RunLock", "another SVXConnect client");
    else
        who = kind;

    if (pid > 0)
        return QCoreApplication::translate("RunLock", "%1 (process %2)").arg(who).arg(pid);
    return who;
}

Result acquire(const QString &lockPath, Holder *out)
{
    const int rc = svx_lock_acquire(qPrintable(lockPath), kOwnerKind);
    if (rc == 0)
        return Result::Acquired;

    if (rc == -1) {
        if (out) {
            /* svx_lock_who() writes into a caller-supplied buffer. The core
             * stores the kind in a char[16], so 16 is the right size here —
             * matching it exactly avoids a truncation that would silently
             * defeat the isPios() / isSelfKind() comparisons above. */
            char kind[16] = {0};
            long pid = 0;
            if (svx_lock_who(qPrintable(lockPath), kind, sizeof kind, &pid) == 0) {
                out->kind = QString::fromUtf8(kind).trimmed();
                out->pid  = pid;
            }
        }
        return Result::HeldByOther;
    }

    return Result::Error;
}

void release()
{
    svx_lock_release();
}

} // namespace RunLock
