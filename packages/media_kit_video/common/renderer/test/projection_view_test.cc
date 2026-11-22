// Immuch360: the tests of the window's view of renderer C (projection_view.h,
// IMMUCH360-NOTE.md, patches 6 and 16), any C++17 compiler, no Windows,
// OpenGL or OpenXR header: run by the analyze job of the CI on Linux.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I.. projection_view_test.cc -o test
//     && ./test
//
// The eye's view and the recentering are pinned by the headset view's tests
// (windows/test/xr_probe_test.cc and xr_v1_test.cc); here the uniforms a
// window's yaw, pitch and field of view give, as the pass reads them:
// - the rows of Ry(yaw) Rx(pitch), a rotation (orthonormal, determinant 1),
//   whose centre ray points at the longitude and latitude asked, the pitch
//   held within -90 and 90 degrees;
// - uViewToSphere column major, as glUniformMatrix3fv reads it;
// - the four tangents of a symmetric field of view on any aspect, the field
//   held within 1 and 170 degrees, a zero height taken as one pixel;
// - the pixels per radian at the centre of the view;
// - an eye's rows and tangents passed through as they are;
// - an eye turned right by an angle sees what the window at that yaw sees.

#include "projection_view.h"

#include <cmath>
#include <cstdio>
#include <string>

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

constexpr double kTight = 1e-9;
constexpr double kFloat = 1e-5;

// rows * (x, y, z)
void Apply(const double rows[9], const double v[3], double out[3]) {
  for (int i = 0; i < 3; i++) {
    out[i] =
        rows[i * 3] * v[0] + rows[i * 3 + 1] * v[1] + rows[i * 3 + 2] * v[2];
  }
}

void IsRotation(const double r[9], const std::string& what) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      double dot = 0;
      for (int k = 0; k < 3; k++) dot += r[i * 3 + k] * r[j * 3 + k];
      Near(i == j ? 1.0 : 0.0, dot, kTight,
           what + " rows " + std::to_string(i) + "." + std::to_string(j));
    }
  }
  const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                     r[1] * (r[3] * r[8] - r[5] * r[6]) +
                     r[2] * (r[3] * r[7] - r[4] * r[6]);
  Near(1.0, det, kTight, what + " determinant");
}

void TestCentreRay() {
  using projection_view::kDegree;
  double rows[9];
  const double ahead[3] = {0, 0, 1};
  double ray[3];

  projection_view::YawPitchRows(0, 0, rows);
  const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (int i = 0; i < 9; i++) Near(identity[i], rows[i], kTight, "yaw 0 pitch 0");

  // The sphere: x right, y up, z towards longitude 0
  for (double yaw : {-170.0, -90.0, -30.0, 45.0, 90.0, 180.0}) {
    for (double pitch : {-60.0, 0.0, 25.0, 89.0}) {
      projection_view::YawPitchRows(yaw, pitch, rows);
      const std::string what =
          "yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch);
      IsRotation(rows, what);
      Apply(rows, ahead, ray);
      // Longitude yaw, latitude pitch
      const double cp = std::cos(pitch * kDegree);
      Near(std::sin(yaw * kDegree) * cp, ray[0], kTight, what + " ray x");
      Near(std::sin(pitch * kDegree), ray[1], kTight, what + " ray y");
      Near(std::cos(yaw * kDegree) * cp, ray[2], kTight, what + " ray z");
      // The view's up stays in the plane of the centre ray and the vertical:
      // no roll
      const double up[3] = {0, 1, 0};
      double view_up[3];
      Apply(rows, up, view_up);
      const double side = ray[2] * view_up[0] - ray[0] * view_up[2];
      Near(0.0, side, 1e-7, what + " no roll");
    }
  }

  // The pitch held at the poles
  projection_view::YawPitchRows(30, 120, rows);
  Apply(rows, ahead, ray);
  Near(1.0, ray[1], kTight, "pitch 120 held at 90");
  projection_view::YawPitchRows(30, -400, rows);
  Apply(rows, ahead, ray);
  Near(-1.0, ray[1], kTight, "pitch -400 held at -90");
}

void TestTangents() {
  double t[4];
  projection_view::SymmetricTangents(90, 2.0, t);
  Near(-2.0, t[0], kTight, "fov 90 aspect 2 left");
  Near(2.0, t[1], kTight, "fov 90 aspect 2 right");
  Near(1.0, t[2], kTight, "fov 90 aspect 2 up");
  Near(-1.0, t[3], kTight, "fov 90 aspect 2 down");

  projection_view::SymmetricTangents(0, 1.0, t);
  Near(std::tan(0.5 * projection_view::kDegree), t[2], kTight, "fov 0 held at 1");
  projection_view::SymmetricTangents(179, 1.0, t);
  Near(std::tan(85 * projection_view::kDegree), t[2], 1e-9, "fov 179 held at 170");
}

void TestUniforms() {
  ProjectionView view;
  view.yaw = 40;
  view.pitch = -15;
  view.fov = 60;
  const auto u = projection_view::UniformsOf(view, 1920, 1080);
  double rows[9];
  projection_view::YawPitchRows(40, -15, rows);
  // Column major: column c, row r at c * 3 + r
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      Near(rows[r * 3 + c], u.columns[c * 3 + r], kFloat,
           "column " + std::to_string(c) + " row " + std::to_string(r));
    }
  }
  const double tan_half = std::tan(30 * projection_view::kDegree);
  Near(-tan_half * 1920.0 / 1080.0, u.tangents[0], kFloat, "16:9 left");
  Near(tan_half * 1920.0 / 1080.0, u.tangents[1], kFloat, "16:9 right");
  Near(tan_half, u.tangents[2], kFloat, "16:9 up");
  Near(-tan_half, u.tangents[3], kFloat, "16:9 down");
  Near(1080.0 / (2 * tan_half), u.pixels_per_radian, 1e-6, "pixels per radian");

  // A window with no height yet: one pixel, no division by zero
  const auto empty = projection_view::UniformsOf(view, 0, 0);
  Check(std::isfinite(empty.tangents[0]) && std::isfinite(empty.pixels_per_radian),
        "an empty output gives finite uniforms");
  Near(0.0, empty.tangents[0], kFloat, "an empty output: aspect 0");

  // An eye: its rows and tangents as they are
  ProjectionView eye;
  eye.eye = true;
  const float rows_in[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
  const float tangents_in[4] = {-1.2f, 0.9f, 1.1f, -1.3f};
  for (int i = 0; i < 9; i++) eye.rows[i] = rows_in[i];
  for (int i = 0; i < 4; i++) eye.tangents[i] = tangents_in[i];
  const auto e = projection_view::UniformsOf(eye, 1000, 1000);
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      Near(rows_in[r * 3 + c], e.columns[c * 3 + r], 0, "eye rows");
    }
  }
  for (int i = 0; i < 4; i++) Near(tangents_in[i], e.tangents[i], 0, "eye tangents");
  Near(1000.0 / (1.1 + 1.3), e.pixels_per_radian, 1e-4, "eye pixels per radian");
}

void TestEyeMatchesWindow() {
  using projection_view::kDegree;
  for (double yaw : {-75.0, -10.0, 0.0, 33.0, 120.0}) {
    // OpenXR: y up, z behind; a head turned right turns by a negative angle
    // about y
    ProjectionView eye;
    projection_view::EyeView(projection_view::AxisAngle(0, 1, 0, -yaw * kDegree),
                             -std::atan(1.0), std::atan(1.0), std::atan(1.0),
                             -std::atan(1.0), 0.0, &eye);
    double rows[9];
    projection_view::YawPitchRows(yaw, 0, rows);
    for (int i = 0; i < 9; i++) {
      Near(rows[i], eye.rows[i], kFloat, "eye turned " + std::to_string(yaw));
    }
    Near(yaw,
         projection_view::HeadYawDegrees(
             projection_view::AxisAngle(0, 1, 0, -yaw * kDegree)),
         1e-6, "head yaw " + std::to_string(yaw));
  }
}

}  // namespace

int main() {
  TestCentreRay();
  TestTangents();
  TestUniforms();
  TestEyeMatchesWindow();
  std::printf("%d of %d checks passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
