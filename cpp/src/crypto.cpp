#include "../include/crypto.h"

BEGIN_LAFRPC_NAMESPACE

QByteArray urandom(int bytes)
{
    QByteArray randomBytes;
    randomBytes.reserve(bytes);
    // TODO sizeof(int) every turn
    for(int i = 0; i < bytes; ++i) {
        uchar c = qrand() % 256;
        randomBytes.append(static_cast<char>(c));
    }
    return randomBytes;
}


Crypto::~Crypto() {}


QByteArray Crypto::genkey(int bytes)
{
    return urandom(bytes);
}

QByteArray DummyCrypto::encrypt(const QByteArray &data, const QByteArray &key)
{
    Q_UNUSED(key);
    return data;
}

QByteArray DummyCrypto::decrypt(const QByteArray &data, const QByteArray &key)
{
    Q_UNUSED(key);
    return data;
}

END_LAFRPC_NAMESPACE
