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
    QByteArray genkey() {return genkey(DEFAULT_KEY_LENATH);}
    virtual QByteArray genkey(int bytes);
    virtual QByteArray encrypt(const QByteArray &data, const QByteArray &key) = 0;
    virtual QByteArray decrypt(const QByteArray &data, const QByteArray &key) = 0;
};


class DummyCrypto: public Crypto
{
public:
    virtual QByteArray encrypt(const QByteArray &data, const QByteArray &key);
    virtual QByteArray decrypt(const QByteArray &data, const QByteArray &key);
};


//class RsaCrypto: public Crypto
//{
//public:
//    virtual QByteArray encrypt(const QByteArray &data, const QByteArray &key);
//    virtual QByteArray decrypt(const QByteArray &data, const QByteArray &key);
//    QByteArray sign(const QByteArray &data);
//    QByteArray verify(const QByteArray &data, const QByteArray &hash);
//};


QByteArray urandom(int bytes);

END_LAFRPC_NAMESPACE

#endif // LAFRPC_TRAN_CRYPTO_H
