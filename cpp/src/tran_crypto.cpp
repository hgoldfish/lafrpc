#include "../include/tran_crypto.h"
#include "qtnetworkng.h"
#include <QtCore/qloggingcategory.h>

static Q_LOGGING_CATEGORY(logger, "lafrpc.crypto");
using namespace qtng;

BEGIN_LAFRPC_NAMESPACE


QByteArray urandom(int bytes)
{
    QByteArray randomBytes;
    randomBytes.reserve(bytes);
    // TODO sizeof(int) every turn
    for (int i = 0; i < bytes; ++i) {
        uchar c = static_cast<uchar>(qrand() % 256);
        randomBytes.append(static_cast<char>(c));
    }
    return randomBytes;
}


Crypto::~Crypto() {}

QByteArray Crypto::encrypt(const QByteArray &data)
{
    return data;
}

QByteArray Crypto::decrypt(const QByteArray &data)
{
    return data;
}


class AES256CryptoPrivate
{
public:
    QByteArray key;
    QByteArray iv;
};


AES256Crypto::AES256Crypto(const QByteArray &key, const QByteArray &iv)
    :d_ptr(new AES256CryptoPrivate())
{
    Q_D(AES256Crypto);
    if (key.size() != 32) {
        qCCritical(logger, "aes256 crypto got key with bad size.");
    } else if (iv.size() != 32) {
        qCCritical(logger, "aes256 crypto got iv with bad size.");
    } else {
        d->key = key;
        d->iv = iv;
    }
}

AES256Crypto::~AES256Crypto()
{
    delete d_ptr;
}

QByteArray AES256Crypto::encrypt(const QByteArray &data)
{
    Q_D(AES256Crypto);
    if (d->key.isEmpty() || d->iv.isEmpty()) {
        return QByteArray();
    }
    QByteArray result;
    Cipher cipher(Cipher::AES256, Cipher::CBC, Cipher::Encrypt);
    cipher.setKey(d->key);
    cipher.setInitialVector(d->iv);
    result = cipher.addData(data);
    result.append(cipher.finalData());
    return result;
}

QByteArray AES256Crypto::decrypt(const QByteArray &data)
{
    Q_D(AES256Crypto);
    if (d->key.isEmpty() || d->iv.isEmpty()) {
        return QByteArray();
    }
    QByteArray result;
    Cipher cipher(Cipher::AES256, Cipher::CBC, Cipher::Decrypt);
    cipher.setKey(d->key);
    cipher.setInitialVector(d->iv);
    result = cipher.addData(data);
    result.append(cipher.finalData());
    return result;
}

END_LAFRPC_NAMESPACE
