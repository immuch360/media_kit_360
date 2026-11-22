// Immuch360: the tests of stabilization.h (IMMUCH360-NOTE.md, patch 14), any
// C++17 compiler, no Windows header: run by the analyze job of the CI.
//
//   g++ -std=c++17 -Wall -Wextra -I.. stabilization_test.cc -o test && ./test
//
// The sign convention is pinned as the Android player pins it
// (StabilizationTableTest.kt of the app): a point of the stitched sphere at
// direction d is shown at K d, so the output direction v samples K^T v. The
// matrix the pass gets is checked as OpenGL reads it (column major), against
// rotations built here apart from the code under test (Rodrigues).

#include "../stabilization.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
  checks++;
  if (!ok) {
    failures++;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void Near(double expected, double actual, double tolerance,
          const std::string& what) {
  Check(std::abs(expected - actual) <= tolerance,
        what + ": expected " + std::to_string(expected) + ", got " +
            std::to_string(actual));
}

const double kDegree = 3.14159265358979323846 / 180.0;

struct Vec {
  double x, y, z;
};

// Rodrigues: the rotation by |angle| about |axis|, right handed, row major
void Rotation(Vec axis, double angle, double m[9]) {
  const double n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  const double x = axis.x / n, y = axis.y / n, z = axis.z / n;
  const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
  const double rows[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                          t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                          t * x * z - s * y, t * y * z + s * x, t * z * z + c};
  for (int i = 0; i < 9; i++) m[i] = rows[i];
}

void Quaternion(Vec axis, double angle, double q[4]) {
  const double n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  const double s = std::sin(angle / 2) / n;
  q[0] = std::cos(angle / 2);
  q[1] = axis.x * s;
  q[2] = axis.y * s;
  q[3] = axis.z * s;
}

Vec Times(const double m[9], Vec v) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
          m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

Vec TransposeTimes(const double m[9], Vec v) {
  return {m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z,
          m[2] * v.x + m[5] * v.y + m[8] * v.z};
}

// What the pass computes, uCorrection * v, with the matrix as glUniformMatrix3fv
// reads |columns| (transpose GL_FALSE: column major)
Vec Sample(const float columns[9], Vec v) {
  return {columns[0] * v.x + columns[3] * v.y + columns[6] * v.z,
          columns[1] * v.x + columns[4] * v.y + columns[7] * v.z,
          columns[2] * v.x + columns[5] * v.y + columns[8] * v.z};
}

void NearVec(Vec expected, Vec actual, double tolerance, const std::string& what) {
  Near(expected.x, actual.x, tolerance, what + " x");
  Near(expected.y, actual.y, tolerance, what + " y");
  Near(expected.z, actual.z, tolerance, what + " z");
}

std::shared_ptr<const stabilization::Table> TableOf(std::vector<int64_t> times,
                                                    std::vector<double> q) {
  std::string reason;
  auto table = stabilization::Table::Of(std::move(times), std::move(q), &reason);
  Check(table != nullptr, "table refused: " + reason);
  return table;
}

std::vector<double> Of(const double q[4]) { return {q[0], q[1], q[2], q[3]}; }

void APitchedCameraHasItsCentreDrawnBelowTheFront() {
  // The camera looks 10 degrees below the horizon and the target is level:
  // the correction turns about x by -10 degrees (y down)
  double k[4];
  Quaternion({1, 0, 0}, -10 * kDegree, k);
  auto table = TableOf({0}, Of(k));
  float m[9];
  table->SampleMatrixAt(0, m);
  // The output direction 10 degrees below the front samples the centre of the
  // stitched picture, what the camera looked at
  const Vec below{0, std::sin(10 * kDegree), std::cos(10 * kDegree)};
  NearVec({0, 0, 1}, Sample(m, below), 1e-6, "pitched: below samples the centre");
  // The front samples the point 10 degrees above the centre: the horizon
  NearVec({0, -std::sin(10 * kDegree), std::cos(10 * kDegree)}, Sample(m, {0, 0, 1}), 1e-6,
          "pitched: the front samples the horizon");
}

void ARolledCameraHasItsHorizonDrawnLevel() {
  // Rolled 15 degrees about its forward axis: the world's right is seen at
  // C^T (1, 0, 0), the first row of C
  double roll[4];
  Quaternion({0, 0, 1}, 15 * kDegree, roll);
  double camera[9];
  Rotation({0, 0, 1}, 15 * kDegree, camera);
  const Vec seen_right{camera[0], camera[1], camera[2]};
  auto table = TableOf({0}, Of(roll));
  float m[9];
  table->SampleMatrixAt(0, m);
  NearVec(seen_right, Sample(m, {1, 0, 0}), 1e-6, "rolled: the right of the view samples the world's right");
}

void AnyCorrectionSamplesKTransposeV() {
  std::mt19937 random(14);
  std::normal_distribution<double> gauss;
  std::uniform_real_distribution<double> uniform(-1.0, 1.0);
  for (int n = 0; n < 50; n++) {
    const Vec axis{gauss(random), gauss(random), gauss(random)};
    const double angle = uniform(random) * 3.14159265358979 * 0.95;
    double q[4];
    Quaternion(axis, angle, q);
    double k[9];
    Rotation(axis, angle, k);
    auto table = TableOf({0}, Of(q));
    float m[9];
    table->SampleMatrixAt(0, m);
    const double negated[4] = {-q[0], -q[1], -q[2], -q[3]};
    auto same = TableOf({0}, Of(negated));
    float m2[9];
    same->SampleMatrixAt(0, m2);
    for (double lon : {-2.5, -1.0, 0.0, 0.7, 2.0}) {
      for (double lat : {-1.2, -0.3, 0.0, 0.5, 1.4}) {
        const Vec v{std::cos(lat) * std::sin(lon), -std::sin(lat), std::cos(lat) * std::cos(lon)};
        const auto what = "random " + std::to_string(n);
        NearVec(TransposeTimes(k, v), Sample(m, v), 1e-5, what);
        NearVec(TransposeTimes(k, v), Sample(m2, v), 1e-5, what + ", -q");
        // The point d of the picture this samples is shown at K d = v
        NearVec(v, Times(k, Sample(m, v)), 1e-5, what + ", K d = v");
      }
    }
  }
}

void AMutatedSignIsCaught() {
  // K instead of K^T: the turn doubles instead of cancelling. The check of
  // AnyCorrectionSamplesKTransposeV must tell it apart.
  double q[4];
  Quaternion({0, 1, 0}, 20 * kDegree, q);
  double k[9];
  Rotation({0, 1, 0}, 20 * kDegree, k);
  auto table = TableOf({0}, Of(q));
  float m[9];
  table->SampleMatrixAt(0, m);
  const Vec v{0, 0, 1};
  const Vec wrong = Times(k, v);
  const Vec right = Sample(m, v);
  Check(std::abs(wrong.x - right.x) > 0.5, "K v and K^T v differ by 40 degrees of turn");
}

void AThreeDegreeTurnIsUndone() {
  // The camera turned 3 degrees about y between frame 0 and frame 1 (from the
  // world to its view: a world point w is seen at d = R w); the target keeps
  // the heading, so K = R^-1 and the output direction w samples d
  double r[9];
  Rotation({0, 1, 0}, 3 * kDegree, r);
  double inverse[4];
  Quaternion({0, 1, 0}, -3 * kDegree, inverse);
  const double identity[4] = {1, 0, 0, 0};
  std::vector<double> q = Of(identity);
  q.insert(q.end(), inverse, inverse + 4);
  auto table = TableOf({0, 33367}, q);
  for (const Vec w : {Vec{0, 0, 1}, Vec{0.6, -0.2, 0.77}, Vec{-0.9, 0.3, -0.3}}) {
    float m[9];
    table->SampleMatrixAt(33367, m);
    NearVec(Times(r, w), Sample(m, w), 1e-6, "turned frame: the world stays");
    table->SampleMatrixAt(0, m);
    NearVec(w, Sample(m, w), 1e-7, "frame 0: identity");
  }
}

void TheLookupBlendsBetweenFrames() {
  const int64_t step = 33367;
  std::vector<int64_t> times;
  std::vector<double> q;
  for (int i = 0; i < 30; i++) {
    times.push_back(i * step);
    double f[4];
    Quaternion({0, 1, 0}, 2.0 * i * kDegree, f);
    // Every other one written as -q: the same rotation, on the other side
    if (i % 2 == 1) {
      for (double& value : f) value = -value;
    }
    q.insert(q.end(), f, f + 4);
  }
  auto table = TableOf(times, q);
  const auto angle_at = [&](int64_t t) {
    double out[4];
    table->QuaternionAt(t, out);
    const double sign = out[0] < 0 ? -1.0 : 1.0;
    return 2.0 * std::atan2(sign * out[2], sign * out[0]) / kDegree;
  };
  Near(14.0, angle_at(7 * step), 1e-6, "frame 7");
  Near(20.5, angle_at(10 * step + step / 4), 1e-3, "a quarter after frame 10");
  Near(21.0, angle_at(10 * step + step / 2), 1e-3, "half way, across a sign flip");
  Near(0.0, angle_at(-500000), 1e-9, "before the first frame");
  Near(58.0, angle_at(10000000), 1e-6, "after the last frame");
}

void TablesThatCannotBeUsedAreRefused() {
  std::string reason;
  Check(stabilization::Table::Of({}, {}, &reason) == nullptr && reason == "no frame", "empty");
  Check(stabilization::Table::Of({0, 1}, {1, 0, 0, 0}, &reason) == nullptr, "lengths");
  Check(stabilization::Table::Of({5, 5}, {1, 0, 0, 0, 1, 0, 0, 0}, &reason) == nullptr, "times not rising");
  Check(stabilization::Table::Of({0}, {2, 0, 0, 0}, &reason) == nullptr, "not unit");
  Check(stabilization::Table::Of({0}, {NAN, 0, 0, 0}, &reason) == nullptr, "not a number");
}

void TheFrameClockFindsTheFrameDrawn() {
  const double period = 1.0 / 30.0;
  const int64_t period_us = 33333;
  stabilization::FrameClock clock;
  // Before any frame drawn in time, late or not, time-pos as it is (the start)
  Near(0.0333, clock.FrameTime(0.0333, 1000000, 1000000 + 30000, 1.0, period), 1e-9, "no reference");
  // In time: time-pos
  Near(1.0, clock.FrameTime(1.0, 2000000, 2000000 + 3000, 1.0, period), 1e-9, "in time");
  // Frame 31 drawn 30 ms late: mpv handed frame 32 over already
  Near(1.0 + period,
       clock.FrameTime(1.0 + 2 * period, 2000000 + period_us, 2000000 + period_us + 30000, 1.0, period), 1e-9,
       "late, the next frame handed over");
  // Late, but mpv had not handed the next frame over: time-pos stays
  Near(1.0 + 2 * period,
       clock.FrameTime(1.0 + 2 * period, 2000000 + 2 * period_us, 2000000 + 2 * period_us + 26000, 1.0, period),
       1e-9, "late, time-pos still the frame's");
  // A seek: the reference says nothing of the new place
  Near(12.5, clock.FrameTime(12.5, 2000000 + 3 * period_us, 2000000 + 3 * period_us + 30000, 1.0, period), 1e-9,
       "after a seek");
  // No target time: time-pos
  Near(3.0, clock.FrameTime(3.0, 0, 5000000, 1.0, period), 1e-9, "no target");
  // Target times in nanoseconds (mpv 0.41), at speed 1.5
  stabilization::FrameClock fast;
  Near(4.0, fast.FrameTime(4.0, 9000000000LL, 9000500, 1.5, period), 1e-9, "ns, in time");
  // Frame period on the wall clock: 22.2 ms; 20 ms late, the next one handed over
  Near(4.0 + period,
       fast.FrameTime(4.0 + 2 * period, 9000000000LL + 22222000LL, 9022222 + 20000, 1.5, period), 1e-9,
       "ns, late at speed 1.5");
  fast.Reset();
  Near(5.0, fast.FrameTime(5.0, 9100000000LL, 9100000 + 30000, 1.5, period), 1e-9, "reset: no reference");
}

}  // namespace

int main() {
  APitchedCameraHasItsCentreDrawnBelowTheFront();
  ARolledCameraHasItsHorizonDrawnLevel();
  AnyCorrectionSamplesKTransposeV();
  AMutatedSignIsCaught();
  AThreeDegreeTurnIsUndone();
  TheLookupBlendsBetweenFrames();
  TablesThatCannotBeUsedAreRefused();
  TheFrameClockFindsTheFrameDrawn();
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
