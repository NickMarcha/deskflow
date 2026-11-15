/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/HeadlessStatus.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QMutex>
#include <QSaveFile>

namespace deskflow::headless {

namespace {
QMutex s_mutex;
QJsonObject s_status;
} // namespace

void updateStatus(const QJsonObject &fields)
{
  static const QString dir = qEnvironmentVariable("DESKFLOW_STATUS_DIR");
  if (dir.isEmpty()) {
    return;
  }

  QMutexLocker lock(&s_mutex);
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    s_status.insert(it.key(), it.value());
  }
  s_status.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

  QDir().mkpath(dir);
  QSaveFile file(dir + QStringLiteral("/status.json"));
  if (file.open(QIODevice::WriteOnly)) {
    file.write(QJsonDocument(s_status).toJson(QJsonDocument::Compact));
    file.commit();
  }
}

} // namespace deskflow::headless
