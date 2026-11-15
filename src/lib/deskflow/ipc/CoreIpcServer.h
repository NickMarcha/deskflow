/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcServer.h"

#include <QObject>
#include <QSet>

#include <functional>

class QLocalSocket;

namespace deskflow::core::ipc {

class CoreIpcServer : public IpcServer
{
  Q_OBJECT

public:
  using KeystrokeReply = std::function<void(const QString &error)>;

  explicit CoreIpcServer(QObject *parent);

  static CoreIpcServer &instance();

Q_SIGNALS:
  /**
   * Emitted for `keystroke=modifiers+key[,computers]` from a client of the same user.
   * The receiver must call \p reply exactly once, from any thread; an empty error means success.
   */
  void keystrokeRequested(const QString &keystroke, const KeystrokeReply &reply);

private:
  void processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts) override;
  void processKeystroke(QLocalSocket *clientSocket, const QString &keystroke);
  void reply(QLocalSocket *clientSocket, const QString &error);
};

} // namespace deskflow::core::ipc
