#include <QtCore/qmap.h>
#include <QtCore/qfile.h>
#include <QtCore/qdatetime.h>
#include "qtnetworkng.h"

using namespace qtng;

int main()
{
    const PrivateKey &key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    QMultiMap<Certificate::SubjectInfo, QString> info;
    info.insert(Certificate::CommonName, "Example");
    info.insert(Certificate::CountryName, "US");
    info.insert(Certificate::Organization, "QtNetworkNg");
    const QDateTime &now = QDateTime::currentDateTime();
    const Certificate &cert = Certificate::generate(key, MessageDigest::Sha256, 293424,
                                                    now, now.addYears(10), info);
    QFile keyfile("test_key.pem");
    keyfile.open(QIODevice::WriteOnly);
    keyfile.write(key.save());
    QFile certfile("test_cert.pem");
    certfile.open(QIODevice::WriteOnly);
    certfile.write(cert.save());
    return 0;
}



