#ifndef LAFRPC_TRAN_CRYPTO_H
#define LAFRPC_TRAN_CRYPTO_H

#include <QtCore/QByteArray>
#include "utils.h"

BEGIN_LAFRPC_NAMESPACE

#ifndef DEFAULT_KEY_LENGTH
#define DEFAULT_KEY_LENATH 64
#endif

class Crypto
{
public:
    virtual ~Crypto();
public:
    virtual QByteArray encrypt(const QByteArray &data);
    virtual QByteArray decrypt(const QByteArray &data);
};

class AES256CryptoPrivate;
class AES256Crypto: public Crypto
{
public:
    AES256Crypto(const QByteArray &key, const QByteArray &iv);
    virtual ~AES256Crypto() override;
public:
    virtual QByteArray encrypt(const QByteArray &data) override;
    virtual QByteArray decrypt(const QByteArray &data) override;
private:
    AES256CryptoPrivate *d_ptr;
    Q_DECLARE_PRIVATE(AES256Crypto)
};



QByteArray urandom(int bytes);

END_LAFRPC_NAMESPACE

#endif // LAFRPC_TRAN_CRYPTO_H
