/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/DirectionTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace deskflow::server {

//! The real monitors of a computer
/*!
Deskflow knows a computer as one rectangle, the outline of all its
monitors. A computer whose monitors don't fill that outline (an offset
monitor, an L-shape) has empty areas in it, where the cursor would travel
off every screen.

A MonitorShape lists the monitors as rectangles in fractions of the
outline (0..1), the way the config's monitors section gives them in
percent. They're scaled to whatever size the client reports, so it doesn't
matter whether its OS counts pixels or scaled units.

With a shape the server keeps the cursor on real monitors, lets it leave
the computer from the outer edge of the monitor it's on, and lands it on
a real monitor when it arrives.
*/
class MonitorShape
{
public:
  struct Rect
  {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    bool operator==(const Rect &) const = default;
  };

  struct PixelRect
  {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    bool contains(int32_t px, int32_t py) const
    {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  //! What a move does on a computer with a shape
  struct Step
  {
    //! Exit: the cursor leaves across \c dir; (x, y) is just past the
    //! outline on that side, ready for the neighbor search. Otherwise it
    //! stays at (x, y), which is on a monitor.
    bool exit = false;
    Direction dir = Direction::NoDirection;
    int32_t x = 0;
    int32_t y = 0;
    //! Where the cursor stays when the exit doesn't happen (no neighbor).
    int32_t stayX = 0;
    int32_t stayY = 0;
  };

  MonitorShape() = default;
  explicit MonitorShape(std::vector<Rect> monitors);

  //! Parses "x,y,w,h x,y,w,h ..." in percent of the outline. Returns false
  //! and sets \c error for anything else.
  static bool parse(const std::string &text, MonitorShape &out, std::string &error);
  //! The text parse() reads.
  std::string format() const;

  //! True without monitors, or with a single monitor filling the outline:
  //! then the outline is the shape and nothing changes.
  bool isPlain() const;
  const std::vector<Rect> &monitors() const
  {
    return m_monitors;
  }
  bool operator==(const MonitorShape &) const = default;

  //! The monitors in the client's coordinates, for an outline at
  //! (ax, ay) of size aw x ah. Monitors that share an edge in fractions
  //! share it in pixels too.
  std::vector<PixelRect> pixels(int32_t ax, int32_t ay, int32_t aw, int32_t ah) const;

  //! True when (x, y) is on a monitor.
  bool contains(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t x, int32_t y) const;

  //! A move from (xOld, yOld) to (x, y) on this computer.
  Step move(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t xOld, int32_t yOld, int32_t x, int32_t y) const;

  //! A cursor arriving at (x, y) while travelling in \c travel (Right
  //! means it came in through the left side) lands on the first monitor
  //! along its way, or the nearest one when none is in its way.
  void enter(int32_t ax, int32_t ay, int32_t aw, int32_t ah, Direction travel, int32_t &x, int32_t &y) const;

  //! Moves (x, y) to the nearest point on a monitor.
  void nearest(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t &x, int32_t &y) const;

private:
  std::vector<Rect> m_monitors;
};

} // namespace deskflow::server
