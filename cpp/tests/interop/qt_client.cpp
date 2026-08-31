// Qt side of the interop test: legacy lafrpc client that connects to the
// std server, calls std.reverse / std.mul, and also serves "demo" so the
// std side can call back.
//
// NOTE: qtng work must run on the qtng event loop (startQtLoop), NOT from a
// QTimer callback — a Qt timer runs on the Qt main coroutine, and blocking
// inside it triggers "yield to myself" and deadlocks the socket I/O.
//
// Usage: ./interop_qt_client <port>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/qtimer.h>
#include <cstdio>

#include "lafrpc.h"
#include "qtnetworkng.h"

static Q_LOGGING_CATEGORY(logger, "interop.qt")

class LegacyDemo: public QObject
{
    Q_OBJECT
public Q_SLOTS:
    QString sayHello(const QString &name) { return QStringLiteral("Hello, %1").arg(name); }
    int add(int a, int b) { return a + b; }
};

class ClientCoroutine: public qtng::Coroutine
{
public:
    ClientCoroutine(quint16 port, int *failures)
        : port(port), failures(failures) {}

    virtual void run() override
    {
        msleep(500);
        QSharedPointer<lafrpc::Rpc> rpc = lafrpc::Rpc::builder(lafrpc::MessagePack)
                .myPeerName(QStringLiteral("qt-side"))
                .create();
        rpc->registerInstance(QSharedPointer<LegacyDemo>::create(), "demo");

        QSharedPointer<lafrpc::Peer> peer =
                rpc->connect(QStringLiteral("tcp://127.0.0.1:%1").arg(port));
        if (peer.isNull()) {
            qCCritical(logger) << "FAIL: qt can not connect to std server.";
            ++(*failures);
            QCoreApplication::exit(1);
            return;
        }
        qCInfo(logger) << "ok: qt connected to std server";

        QVariant r1 = peer->call(QStringLiteral("std.reverse"), QVariant(QStringLiteral("abcdef")));
        if (r1.toString() == QStringLiteral("fedcba")) {
            qCInfo(logger) << "ok: qt->std std.reverse";
        } else {
            qCCritical(logger) << "FAIL: qt->std std.reverse =" << r1;
            ++(*failures);
        }

        QVariant r2 = peer->call(QStringLiteral("std.mul"), QVariant(6), QVariant(7));
        if (r2.toLongLong() == 42) {
            qCInfo(logger) << "ok: qt->std std.mul";
        } else {
            qCCritical(logger) << "FAIL: qt->std std.mul =" << r2;
            ++(*failures);
        }

        // give the reverse direction a moment, then report
        msleep(1500);
        qCInfo(logger) << (*failures == 0 ? "QT_SIDE_ALL_OK" : "QT_SIDE_FAILED");
        QCoreApplication::exit(*failures == 0 ? 0 : 1);
    }

private:
    quint16 port;
    int *failures;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    quint16 port = 17942;
    if (argc > 1) {
        port = static_cast<quint16>(atoi(argv[1]));
    }

    int failures = 0;
    qtng::CoroutineGroup operations;
    operations.start(new ClientCoroutine(port, &failures), "client");
    operations.joinall();
    return failures == 0 ? 0 : 1;
}

#include "qt_client.moc"
