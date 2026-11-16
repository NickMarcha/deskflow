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

  /**
   * Emitted for `switch=computer` from a client of the same user: make that computer the active one.
   * The receiver must call \p reply exactly once, from any thread; an empty error means success.
   */
  void switchRequested(const QString &computer, const KeystrokeReply &reply);

public:
  /**
   * Broadcasts `action=name` (a notify(name) hotkey action fired) to the connected clients.
   * Dropped when nobody is connected: a hotkey pressed earlier must not run later. Call from
   * any thread.
   */
  void notifyAction(const QString &name);

private:
  void processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts) override;
  void processKeystroke(QLocalSocket *clientSocket, const QString &keystroke);
  void processSwitch(QLocalSocket *clientSocket, const QString &computer);
  void reply(QLocalSocket *clientSocket, const QString &error);
};

} // namespace deskflow::core::ipc
