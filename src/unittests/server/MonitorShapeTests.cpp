/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorShapeTests.h"

#include "server/MonitorShape.h"

using deskflow::server::MonitorShape;
using enum Direction;

namespace {

// A 3440x1440 ultrawide at 0,0 and a 1920x1080 monitor on its right, 356 px
// lower: a 5360x1440 outline with a gap above the right monitor.
constexpr int32_t W = 5360;
constexpr int32_t H = 1440;

MonitorShape desk()
{
  MonitorShape shape;
  std::string error;
  const bool ok = MonitorShape::parse("0,0,64.1791,100 64.1791,24.7222,35.8209,75", shape, error);
  Q_ASSERT(ok);
  return shape;
}

} // namespace

void MonitorShapeTests::pixelsShareEdges()
{
  const auto rects = desk().pixels(0, 0, W, H);
  QCOMPARE(rects.size(), size_t(2));
  QCOMPARE(rects[0].x, 0);
  QCOMPARE(rects[0].w, 3440);
  QCOMPARE(rects[0].h, 1440);
  QCOMPARE(rects[1].x, 3440); // no gap and no overlap between them
  QCOMPARE(rects[1].y, 356);
  QCOMPARE(rects[1].w, 1920);
  QCOMPARE(rects[1].h, 1080);
}

void MonitorShapeTests::parseAndFormat()
{
  MonitorShape shape;
  std::string error;
  QVERIFY(MonitorShape::parse("  0,0,50,100   50,25,50,75 ", shape, error));
  QCOMPARE(shape.format(), std::string("0,0,50,100 50,25,50,75"));
  MonitorShape again;
  QVERIFY(MonitorShape::parse(shape.format(), again, error));
  QVERIFY(again == shape);
}

void MonitorShapeTests::parseErrors()
{
  MonitorShape shape;
  std::string error;
  QVERIFY(!MonitorShape::parse("", shape, error));
  QVERIFY(!MonitorShape::parse("0,0,50", shape, error));
  QVERIFY(!MonitorShape::parse("0,0,50,100x", shape, error));
  QVERIFY(!MonitorShape::parse("0;0;50;100", shape, error));
  QVERIFY(!MonitorShape::parse("60,0,50,100", shape, error)); // past 100 %
  QVERIFY(!MonitorShape::parse("0,0,0,100", shape, error));   // no width
  QVERIFY(!error.empty());
}

void MonitorShapeTests::plainShapes()
{
  MonitorShape shape;
  std::string error;
  QVERIFY(MonitorShape().isPlain());
  QVERIFY(MonitorShape::parse("0,0,100,100", shape, error));
  QVERIFY(shape.isPlain());
  QVERIFY(MonitorShape::parse("0,0,50,100 50,0,50,100", shape, error));
  QVERIFY(!shape.isPlain()); // two monitors, even side by side
  QVERIFY(!desk().isPlain());
}

void MonitorShapeTests::movesStayOnMonitors()
{
  // Moving right into the gap above the right monitor stops at the
  // ultrawide's edge when the computer has no neighbour there.
  const auto step = desk().move(0, 0, W, H, 3400, 100, 3500, 100);
  QVERIFY(step.exit); // an outer edge: the server tries the neighbour first
  QCOMPARE(step.stayX, 3439);
  QCOMPARE(step.stayY, 100);

  const auto inside = desk().move(0, 0, W, H, 100, 100, 200, 300);
  QVERIFY(!inside.exit);
  QCOMPARE(inside.x, 200);
  QCOMPARE(inside.y, 300);
}

void MonitorShapeTests::movesBetweenMonitors()
{
  const auto step = desk().move(0, 0, W, H, 3400, 500, 3500, 500);
  QVERIFY(!step.exit);
  QCOMPARE(step.x, 3500);
  QCOMPARE(step.y, 500);
}

void MonitorShapeTests::leavesFromTheMonitorEdge()
{
  // Right from the ultrawide, above the right monitor: an exit on the
  // right, at the same height, just past the outline.
  auto step = desk().move(0, 0, W, H, 3400, 100, 3500, 100);
  QVERIFY(step.exit);
  QCOMPARE(step.dir, Right);
  QCOMPARE(step.x, W);
  QCOMPARE(step.y, 100);

  // Up from the right monitor's top edge, which is inside the outline.
  step = desk().move(0, 0, W, H, 4000, 360, 4000, 350);
  QVERIFY(step.exit);
  QCOMPARE(step.dir, Top);
  QCOMPARE(step.x, 4000);
  QCOMPARE(step.y, -1);
  QCOMPARE(step.stayY, 356);

  // Down from the right monitor's bottom edge, 4 px above the outline's.
  step = desk().move(0, 0, W, H, 4000, 1430, 4000, 1438);
  QVERIFY(step.exit);
  QCOMPARE(step.dir, Bottom);
  QCOMPARE(step.y, H);
  QCOMPARE(step.stayY, 1435);

  // Left from the ultrawide is the outline's own edge, as before.
  step = desk().move(0, 0, W, H, 5, 700, -3, 700);
  QVERIFY(step.exit);
  QCOMPARE(step.dir, Left);
  QCOMPARE(step.x, -1);
}

void MonitorShapeTests::diagonalIntoTheGap()
{
  // Up and right from the right monitor into the gap above it: it leaves
  // across the top, the side the move crossed.
  const auto step = desk().move(0, 0, W, H, 3450, 360, 3460, 340);
  QVERIFY(step.exit);
  QCOMPARE(step.dir, Top);
  QCOMPARE(step.x, 3460);
  QCOMPARE(step.stayY, 356);
}

void MonitorShapeTests::scalesToTheClientSize()
{
  // The same monitors, reported by a client that counts in other units.
  const auto rects = desk().pixels(100, 50, W / 2, H / 2);
  QCOMPARE(rects[1].x, 100 + 1720);
  QCOMPARE(rects[1].y, 50 + 178);
  QVERIFY(desk().contains(100, 50, W / 2, H / 2, 100 + 1800, 50 + 300));
  QVERIFY(!desk().contains(100, 50, W / 2, H / 2, 100 + 1800, 50 + 100));
}

void MonitorShapeTests::entersOnTheFirstMonitorInTheWay()
{
  // Coming in from the right above the right monitor: onto the ultrawide.
  int32_t x = W - 1;
  int32_t y = 100;
  desk().enter(0, 0, W, H, Left, x, y);
  QCOMPARE(x, 3439);
  QCOMPARE(y, 100);

  // Coming in from the right lower down: the right monitor, untouched.
  x = W - 1;
  y = 700;
  desk().enter(0, 0, W, H, Left, x, y);
  QCOMPARE(x, W - 1);

  // Coming in from the top above the right monitor: onto its top edge.
  x = 4000;
  y = 0;
  desk().enter(0, 0, W, H, Bottom, x, y);
  QCOMPARE(x, 4000);
  QCOMPARE(y, 356);
}

void MonitorShapeTests::nearestMonitor()
{
  int32_t x = 4000;
  int32_t y = 100;
  desk().nearest(0, 0, W, H, x, y);
  QCOMPARE(x, 4000);
  QCOMPARE(y, 356);

  x = 1000;
  y = 700;
  desk().nearest(0, 0, W, H, x, y);
  QCOMPARE(x, 1000);
  QCOMPARE(y, 700);
}

QTEST_MAIN(MonitorShapeTests)
