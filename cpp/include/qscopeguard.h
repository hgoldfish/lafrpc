// qscopeguard.h — 兼容 Qt < 5.12 的 QScopeGuard 实现。
// 仅当系统 Qt 低于 5.12 时由 lafrpc 源码包含；Qt >= 5.12 使用 QtCore 的原生实现。
#ifndef LAFRPC_QSCOPEGUARD_COMPAT_H
#define LAFRPC_QSCOPEGUARD_COMPAT_H

#include <QtCore/qglobal.h>
#include <utility>

template <typename F>
class QScopeGuard
{
public:
    explicit QScopeGuard(F &&f) Q_DECL_NOTHROW
        : m_func(std::forward<F>(f)), m_active(true)
    {
    }

    QScopeGuard(const QScopeGuard &) = delete;
    QScopeGuard &operator=(const QScopeGuard &) = delete;

    QScopeGuard(QScopeGuard &&other) Q_DECL_NOTHROW
        : m_func(std::move(other.m_func)), m_active(other.m_active)
    {
        other.m_active = false;
    }

    ~QScopeGuard()
    {
        if (m_active) {
            m_func();
        }
    }

    void dismiss() Q_DECL_NOTHROW { m_active = false; }

private:
    F m_func;
    bool m_active;
};

template <typename Function>
QScopeGuard<Function> qScopeGuard(Function f)
{
    return QScopeGuard<Function>(std::forward<Function>(f));
}

#endif  // LAFRPC_QSCOPEGUARD_COMPAT_H
