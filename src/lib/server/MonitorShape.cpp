/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/MonitorShape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace deskflow::server {

namespace {

int32_t edge(int32_t origin, int32_t size, double fraction)
{
  return origin + static_cast<int32_t>(std::lround(fraction * size));
}

int32_t clampTo(int32_t v, int32_t low, int32_t high)
{
  return v < low ? low : (v > high ? high : v);
}

int64_t distance2(const MonitorShape::PixelRect &r, int32_t x, int32_t y)
{
  const int64_t dx = static_cast<int64_t>(clampTo(x, r.x, r.x + r.w - 1)) - x;
  const int64_t dy = static_cast<int64_t>(clampTo(y, r.y, r.y + r.h - 1)) - y;
  return dx * dx + dy * dy;
}

bool onAny(const std::vector<MonitorShape::PixelRect> &rects, int32_t x, int32_t y)
{
  return std::any_of(rects.begin(), rects.end(), [x, y](const auto &r) { return r.contains(x, y); });
}

} // namespace

MonitorShape::MonitorShape(std::vector<Rect> monitors) : m_monitors(std::move(monitors))
{
  // nothing else
}

bool MonitorShape::parse(const std::string &text, MonitorShape &out, std::string &error)
{
  std::vector<Rect> monitors;
  std::istringstream words(text);
  std::string word;
  while (words >> word) {
    Rect r;
    char c1 = 0;
    char c2 = 0;
    char c3 = 0;
    std::istringstream parts(word);
    parts.imbue(std::locale::classic());
    if (!(parts >> r.x >> c1 >> r.y >> c2 >> r.w >> c3 >> r.h) || c1 != ',' || c2 != ',' || c3 != ',' ||
        parts.peek() != std::char_traits<char>::eof()) {
      error = "expected x,y,w,h in percent, got \"" + word + "\"";
      return false;
    }
    if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + r.w > 100.001 || r.y + r.h > 100.001) {
      error = "monitor \"" + word + "\" is outside 0..100";
      return false;
    }
    monitors.push_back({r.x / 100.0, r.y / 100.0, r.w / 100.0, r.h / 100.0});
  }
  if (monitors.empty()) {
    error = "no monitors";
    return false;
  }
  out = MonitorShape(std::move(monitors));
  return true;
}

std::string MonitorShape::format() const
{
  std::string out;
  char buffer[128];
  for (const auto &r : m_monitors) {
    std::snprintf(
        buffer, sizeof(buffer), "%s%g,%g,%g,%g", out.empty() ? "" : " ", r.x * 100.0, r.y * 100.0, r.w * 100.0,
        r.h * 100.0
    );
    out += buffer;
  }
  return out;
}

bool MonitorShape::isPlain() const
{
  if (m_monitors.empty()) {
    return true;
  }
  if (m_monitors.size() > 1) {
    return false;
  }
  const auto &r = m_monitors.front();
  return r.x < 0.0001 && r.y < 0.0001 && r.w > 0.9999 && r.h > 0.9999;
}

std::vector<MonitorShape::PixelRect> MonitorShape::pixels(int32_t ax, int32_t ay, int32_t aw, int32_t ah) const
{
  std::vector<PixelRect> out;
  out.reserve(m_monitors.size());
  for (const auto &r : m_monitors) {
    const int32_t left = edge(ax, aw, r.x);
    const int32_t top = edge(ay, ah, r.y);
    const int32_t right = edge(ax, aw, r.x + r.w);
    const int32_t bottom = edge(ay, ah, r.y + r.h);
    if (right > left && bottom > top) {
      out.push_back({left, top, right - left, bottom - top});
    }
  }
  return out;
}

bool MonitorShape::contains(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t x, int32_t y) const
{
  return onAny(pixels(ax, ay, aw, ah), x, y);
}

MonitorShape::Step MonitorShape::move(
    int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t xOld, int32_t yOld, int32_t x, int32_t y
) const
{
  Step step;
  step.x = step.stayX = x;
  step.y = step.stayY = y;
  const auto rects = pixels(ax, ay, aw, ah);
  if (rects.empty() || onAny(rects, x, y)) {
    return step;
  }

  // The monitor the cursor was on (or the nearest, if it was off them all).
  const PixelRect *from = nullptr;
  int64_t best = std::numeric_limits<int64_t>::max();
  for (const auto &r : rects) {
    if (const auto d = distance2(r, xOld, yOld); d < best) {
      best = d;
      from = &r;
    }
  }
  const int32_t cx = clampTo(x, from->x, from->x + from->w - 1);
  const int32_t cy = clampTo(y, from->y, from->y + from->h - 1);
  step.stayX = cx;
  step.stayY = cy;

  // Which sides of that monitor the move crossed. A side with no monitor
  // just past it, where the cursor crosses, is an outer edge: leave the
  // computer there as if from its outline.
  using enum Direction;
  const Direction horizontal = x < from->x ? Left : (x >= from->x + from->w ? Right : NoDirection);
  const Direction vertical = y < from->y ? Top : (y >= from->y + from->h ? Bottom : NoDirection);
  for (const Direction dir : {horizontal, vertical}) {
    int32_t px = cx;
    int32_t py = cy;
    switch (dir) {
    case Left:
      px = from->x - 1;
      break;
    case Right:
      px = from->x + from->w;
      break;
    case Top:
      py = from->y - 1;
      break;
    case Bottom:
      py = from->y + from->h;
      break;
    default:
      continue;
    }
    if (onAny(rects, px, py)) {
      continue; // another monitor is there: a corner of the gap, not an exit
    }
    step.exit = true;
    step.dir = dir;
    step.x = dir == Left ? ax - 1 : (dir == Right ? ax + aw : cx);
    step.y = dir == Top ? ay - 1 : (dir == Bottom ? ay + ah : cy);
    return step;
  }

  step.x = cx;
  step.y = cy;
  return step;
}

void MonitorShape::enter(int32_t ax, int32_t ay, int32_t aw, int32_t ah, Direction travel, int32_t &x, int32_t &y) const
{
  const auto rects = pixels(ax, ay, aw, ah);
  if (rects.empty() || onAny(rects, x, y)) {
    return;
  }
  using enum Direction;
  const bool horizontal = travel == Left || travel == Right;
  // The first monitor in the cursor's way: one covering its row (or
  // column), nearest to the side it came in through.
  const PixelRect *hit = nullptr;
  for (const auto &r : rects) {
    const bool inLine = horizontal ? (y >= r.y && y < r.y + r.h) : (x >= r.x && x < r.x + r.w);
    if (!inLine) {
      continue;
    }
    if (hit == nullptr || (travel == Right && r.x < hit->x) || (travel == Left && r.x + r.w > hit->x + hit->w) ||
        (travel == Bottom && r.y < hit->y) || (travel == Top && r.y + r.h > hit->y + hit->h)) {
      hit = &r;
    }
  }
  if (hit != nullptr) {
    if (horizontal) {
      x = clampTo(x, hit->x, hit->x + hit->w - 1);
    } else {
      y = clampTo(y, hit->y, hit->y + hit->h - 1);
    }
    return;
  }
  // Nothing in its way: the nearest monitor, entered from the same side.
  nearest(ax, ay, aw, ah, x, y);
}

void MonitorShape::nearest(int32_t ax, int32_t ay, int32_t aw, int32_t ah, int32_t &x, int32_t &y) const
{
  const auto rects = pixels(ax, ay, aw, ah);
  const PixelRect *best = nullptr;
  int64_t bestDistance = std::numeric_limits<int64_t>::max();
  for (const auto &r : rects) {
    if (const auto d = distance2(r, x, y); d < bestDistance) {
      bestDistance = d;
      best = &r;
    }
  }
  if (best != nullptr) {
    x = clampTo(x, best->x, best->x + best->w - 1);
    y = clampTo(y, best->y, best->y + best->h - 1);
  }
}

} // namespace deskflow::server
