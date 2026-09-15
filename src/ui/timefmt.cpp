/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/timefmt.h"

namespace TimeFmt {

QString duration(quint64 seconds)
{
    const quint64 h = seconds / 3600;
    const quint64 m = (seconds % 3600) / 60;
    const quint64 s = seconds % 60;

    if (h > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    if (m > 0)
        return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));

    /* Under a minute, seconds read better than "0:07" in a narrow column. */
    return QCoreApplication::translate("TimeFmt", "%1s").arg(s);
}

QString elapsed(quint64 startMs, quint64 nowMs)
{
    if (startMs == 0 || nowMs < startMs)
        return duration(0);
    return duration((nowMs - startMs) / 1000);
}

QString ago(quint64 stampMs, quint64 nowMs)
{
    if (stampMs == 0)
        return QString();
    if (nowMs <= stampMs)
        return QCoreApplication::translate("TimeFmt", "just now");

    const quint64 s = (nowMs - stampMs) / 1000;
    if (s < 5)     return QCoreApplication::translate("TimeFmt", "just now");
    if (s < 60)    return QCoreApplication::translate("TimeFmt", "%1s ago").arg(s);
    if (s < 3600)  return QCoreApplication::translate("TimeFmt", "%1m ago").arg(s / 60);
    if (s < 86400) return QCoreApplication::translate("TimeFmt", "%1h ago").arg(s / 3600);
    return QCoreApplication::translate("TimeFmt", "%1d ago").arg(s / 86400);
}

QString compactAge(quint64 stampMs, quint64 nowMs)
{
    if (stampMs == 0)
        return QStringLiteral("—");
    if (nowMs <= stampMs)
        return QCoreApplication::translate("TimeFmt", "now");

    const quint64 s = (nowMs - stampMs) / 1000;
    if (s < 60)    return QCoreApplication::translate("TimeFmt", "%1s").arg(s);
    if (s < 3600)  return QCoreApplication::translate("TimeFmt", "%1m").arg(s / 60);
    if (s < 86400) return QCoreApplication::translate("TimeFmt", "%1h").arg(s / 3600);
    return QCoreApplication::translate("TimeFmt", "%1d").arg(s / 86400);
}

} // namespace TimeFmt
