/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QJsonObject>

namespace deskflow::headless {

//! Merges \p fields into $DESKFLOW_STATUS_DIR/status.json.
/*!
The headless server's web UI reads this file to show connected clients, the
active computer, input devices and the result of the last config reload.
Does nothing when DESKFLOW_STATUS_DIR is unset.
*/
void updateStatus(const QJsonObject &fields);

} // namespace deskflow::headless
