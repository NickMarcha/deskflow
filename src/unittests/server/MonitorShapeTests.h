/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class MonitorShapeTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void pixelsShareEdges();
  void parseAndFormat();
  void parseErrors();
  void plainShapes();
  void movesStayOnMonitors();
  void movesBetweenMonitors();
  void leavesFromTheMonitorEdge();
  void diagonalIntoTheGap();
  void scalesToTheClientSize();
  void entersOnTheFirstMonitorInTheWay();
  void nearestMonitor();
};
