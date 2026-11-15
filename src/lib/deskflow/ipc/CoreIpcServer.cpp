/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcServer.h"

#include "base/Log.h"
#include "common/Constants.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QPointer>

#if !defined(Q_OS_WIN)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace deskflow::core::ipc {

static CoreIpcServer *s_instance = nullptr;

// The socket is world-accessible (see IpcServer::listen), so commands that inject input
// are only accepted from processes running as the same user.
static bool isSameUser(const QLocalSocket *socket)
{
#if defined(Q_OS_LINUX)
  ucred cred{};
  socklen_t length = sizeof(cred);
  const auto fd = static_cast<int>(socket->socketDescriptor());
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) == 0 && cred.uid == geteuid();
#elif defined(Q_OS_UNIX)
  uid_t uid = 0;
  gid_t gid = 0;
  return getpeereid(static_cast<int>(socket->socketDescriptor()), &uid, &gid) == 0 && uid == geteuid();
#else
  Q_UNUSED(socket)
  return false;
#endif
}

CoreIpcServer::CoreIpcServer(QObject *parent) : IpcServer(parent, kCoreIpcName, QStringLiteral("core"))
{
  assert(s_instance == nullptr);
  s_instance = this;
}

CoreIpcServer &CoreIpcServer::instance()
{
  assert(s_instance != nullptr);
  return *s_instance;
}

void CoreIpcServer::processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts)
{
  if (command == QStringLiteral("stop")) {
    LOG_DEBUG("core ipc server got stop message");
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
    broadcastCommand(QStringLiteral("bye"));
    Q_EMIT stopProcessRequested();
    return;
  }
  if (command == QStringLiteral("keystroke")) {
    processKeystroke(clientSocket, parts.mid(1).join('='));
    return;
  }
  if (command == QStringLiteral("switch")) {
    processSwitch(clientSocket, parts.mid(1).join('='));
    return;
  }
  LOG_WARN("core ipc server got unknown command: %s", command.toUtf8().constData());
}

void CoreIpcServer::processKeystroke(QLocalSocket *clientSocket, const QString &keystroke)
{
  if (!isSameUser(clientSocket)) {
    LOG_WARN("core ipc server refused keystroke from another user");
    writeToClientSocket(clientSocket, QStringLiteral("error=permission denied"));
    return;
  }
  if (!isSignalConnected(QMetaMethod::fromSignal(&CoreIpcServer::keystrokeRequested))) {
    writeToClientSocket(clientSocket, QStringLiteral("error=keystroke needs server mode"));
    return;
  }

  LOG_DEBUG("core ipc server got keystroke: %s", keystroke.toUtf8().constData());
  QPointer<CoreIpcServer> self(this);
  QPointer<QLocalSocket> socket(clientSocket);
  Q_EMIT keystrokeRequested(keystroke, [self, socket](const QString &error) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self, socket, error] {
          if (self && socket) {
            self->reply(socket, error);
          }
        },
        Qt::QueuedConnection
    );
  });
}

void CoreIpcServer::processSwitch(QLocalSocket *clientSocket, const QString &computer)
{
  if (!isSameUser(clientSocket)) {
    LOG_WARN("core ipc server refused switch from another user");
    writeToClientSocket(clientSocket, QStringLiteral("error=permission denied"));
    return;
  }
  if (!isSignalConnected(QMetaMethod::fromSignal(&CoreIpcServer::switchRequested))) {
    writeToClientSocket(clientSocket, QStringLiteral("error=switch needs server mode"));
    return;
  }

  LOG_DEBUG("core ipc server got switch: %s", computer.toUtf8().constData());
  QPointer<CoreIpcServer> self(this);
  QPointer<QLocalSocket> socket(clientSocket);
  Q_EMIT switchRequested(computer, [self, socket](const QString &error) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self, socket, error] {
          if (self && socket) {
            self->reply(socket, error);
          }
        },
        Qt::QueuedConnection
    );
  });
}

void CoreIpcServer::reply(QLocalSocket *clientSocket, const QString &error)
{
  writeToClientSocket(clientSocket, error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("error=%1").arg(error));
  clientSocket->flush();
}

} // namespace deskflow::core::ipc
