/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class EvdevDeviceFilterTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void splitList_spacesAndEmptyItems_trimmedAndDropped();
  void keyboardAndMouse_noFilter_used();
  void keysWithSibling_noFilter_used();
  void headsetKeys_noKeysList_skipped();
  void headsetKeys_inKeysList_used();
  void headsetKeys_inKeysAndIgnore_filtered();
  void headsetKeys_inKeysNotInMatch_filtered();
  void pointingStickOnly_inKeysList_skipped();
  void match_unchangedForKeyboards();
  void findBinding_exactBeatsPartBeatsAny();
  void findBinding_noMatch_minusOne();
};
