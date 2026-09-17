/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "net/qrzlookup.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include "core/svxcore.h"   /* log_info */

namespace {

/* A record older than this is looked up again. ham-tools never expires
 * anything — it writes `updated` and never reads it — so the policy has to
 * live here. A month is what the macOS app uses, and a QRZ entry changes about
 * as often as somebody moves house. */
constexpr qint64 kMaxAgeSec = 30LL * 24 * 60 * 60;

/* And a short one for "QRZ has never heard of this callsign", which is the
 * normal answer for half the nodes on a reflector. */
constexpr qint64 kMissAgeSec = 600;

QString hamToolsDir()
{
    return QDir::homePath() + QStringLiteral("/.config/ham-tools");
}

/* The inverse of ham-tools' own escaping (c/qrz/fields.c): a backslash escapes
 * itself and n means a newline. Nothing else is special. */
QString unescape(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        if (in[i] != QLatin1Char('\\') || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        const QChar next = in[++i];
        out += (next == QLatin1Char('n')) ? QLatin1Char('\n') : next;
    }
    return out;
}

} // namespace

QrzLookup::QrzLookup(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<QrzLookup::Record>("QrzLookup::Record");
}

/* ------------------------------------------------------------- the install */

QString QrzLookup::binaryPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("qrz"));
}

QString QrzLookup::configPath()
{
    return hamToolsDir() + QStringLiteral("/config.yaml");
}

QString QrzLookup::cachePath()
{
    return hamToolsDir() + QStringLiteral("/qrz.db");
}

bool QrzLookup::available()
{
    /* Both halves matter: the command without a configuration cannot log in to
     * QRZ, and every lookup would cost a second to fail. */
    return !binaryPath().isEmpty() && QFileInfo::exists(configPath());
}

/* --------------------------------------------------------------- the key */

QString QrzLookup::homeCall(const QString &raw)
{
    QString call = raw.trimmed().toUpper();
    if (call.isEmpty())
        return call;

    /* An SSID is this network's idea, not QRZ's: ON3URE-7 is ON3URE with a
     * particular radio. */
    call = call.section(QLatin1Char('-'), 0, 0);

    /* One trailing operating suffix. /P and /M say where somebody is, not who
     * they are. */
    static const char *kSuffixes[] = { "/MM", "/AM", "/QRP", "/P", "/M", "/A", "/B", "/T" };
    for (const char *suffix : kSuffixes) {
        if (call.endsWith(QLatin1String(suffix))) {
            call.chop(int(strlen(suffix)));
            break;
        }
    }

    if (!call.contains(QLatin1Char('/')))
        return call;

    /* What is left is a prefixed or linked callsign: F/ON3XYZ, or the
     * reflector's own ON0CK/ON3TTR for a station heard through a node. The
     * operator is the part with a digit in it; the last such, because the
     * second half is the station and the first is where it is. */
    const QStringList parts = call.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString best;
    for (const QString &part : parts) {
        bool hasDigit = false;
        for (const QChar c : part)
            if (c.isDigit()) { hasDigit = true; break; }
        if (hasDigit)
            best = part;
    }
    if (!best.isEmpty())
        return best;

    for (const QString &part : parts)
        if (part.size() > best.size())
            best = part;
    return best;
}

/* -------------------------------------------------------------- the data */

QHash<QString, QString> QrzLookup::parseFields(const QString &data)
{
    QHash<QString, QString> out;
    const QStringList lines = data.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        out.insert(line.left(eq), unescape(line.mid(eq + 1)));
    }
    return out;
}

QrzLookup::Record QrzLookup::recordFrom(const QString &homeCall,
                                        const QHash<QString, QString> &fields)
{
    Record r;
    if (fields.isEmpty())
        return r;

    r.callsign = homeCall;

    QStringList name;
    if (!fields.value(QStringLiteral("fname")).isEmpty())
        name << fields.value(QStringLiteral("fname"));
    if (!fields.value(QStringLiteral("name")).isEmpty())
        name << fields.value(QStringLiteral("name"));
    r.fullName = name.join(QLatin1Char(' '));

    r.city         = fields.value(QStringLiteral("addr2"));
    r.country      = fields.value(QStringLiteral("country"));
    r.grid         = fields.value(QStringLiteral("grid"));
    r.licenceClass = fields.value(QStringLiteral("class"));
    r.email        = fields.value(QStringLiteral("email"));
    return r;
}

/* ------------------------------------------------------------- the lookup */

bool QrzLookup::readCache(const QString &call, Record *out) const
{
    const QString path = cachePath();
    if (!QFileInfo::exists(path))
        return false;

    /* A private connection per read, opened read-only. The database belongs to
     * ham-tools and may be being written while this runs; read-only means this
     * can never be the process that corrupts it. */
    const QString name = QStringLiteral("svxqrz-%1").arg(quintptr(this));
    bool found = false;
    {
        QSqlDatabase db = QSqlDatabase::contains(name)
                        ? QSqlDatabase::database(name, false)
                        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open())
            return false;

        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT data, updated FROM qrz WHERE call = ?"));
        q.addBindValue(call);
        if (q.exec() && q.next()) {
            const qint64 updated = q.value(1).toLongLong();
            if (QDateTime::currentSecsSinceEpoch() - updated <= kMaxAgeSec) {
                *out  = recordFrom(call, parseFields(q.value(0).toString()));
                found = out->isValid();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return found;
}

void QrzLookup::lookup(const QString &callsign)
{
    if (!available())
        return;

    const QString call = homeCall(callsign);
    if (call.isEmpty())
        return;

    Record cached;
    if (readCache(call, &cached)) {
        /* Queued, not immediate: a caller that opens a card and then asks is
         * entitled to receive the answer after its own code has finished. */
        QTimer::singleShot(0, this, [this, call, cached]() { emit resolved(call, cached); });
        return;
    }

    if (m_inFlight.contains(call))
        return;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - m_missedAt.value(call, 0) < kMissAgeSec)
        return;   /* QRZ said no recently; do not ask again for a while */

    runQrz(call);
}

void QrzLookup::runQrz(const QString &call)
{
    m_inFlight.insert(call);

    auto *proc = new QProcess(this);
    proc->setProgram(binaryPath());
    /* No flags. `qrz --version` would be looked up as a callsign — ham-tools
     * treats every argument as one. */
    proc->setArguments({call});
    proc->setProcessChannelMode(QProcess::SeparateChannels);

    /* finished() and errorOccurred() are not exclusive — a process that dies
     * emits both, one that never starts emits only the second — so whichever
     * arrives first claims the lookup and the other returns. */
    auto finish = [this, proc, call]() {
        if (!m_inFlight.remove(call))
            return;
        proc->deleteLater();

        Record r;
        if (readCache(call, &r)) {
            emit resolved(call, r);
            return;
        }

        /* Nothing in the cache: QRZ does not know this callsign, the
         * credentials are missing, or the network is down. All three are
         * silent as far as the map is concerned — a station simply has no
         * operator details. */
        m_missedAt.insert(call, QDateTime::currentSecsSinceEpoch());
        emit resolved(call, Record{});
    };

    connect(proc, &QProcess::finished, this,
            [finish](int, QProcess::ExitStatus) { finish(); });
    connect(proc, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError) { finish(); });

    proc->start();
}
