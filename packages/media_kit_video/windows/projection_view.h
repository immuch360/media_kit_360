// Immuch360: the view of renderer C, and its uniforms, apart from any OpenGL
// or Windows header so that the CI tests it with any C++17 compiler
// (IMMUCH360-NOTE.md, patches 6 and 15).
//
// A view is what the pass needs to turn a pixel of the output into a ray of
// the sphere: the rotation of the view's frame into the sphere's, and the four
// tangents of its field of view. The window gives a yaw, a pitch and a vertical
// field of view (symmetric); a headset gives, per eye, an orientation
// quaternion and four angles, often asymmetric (OpenXR's XrPosef and XrFovf).
// Both end as the same uniforms: uViewToSphere and uTanFov.
//
// Frames:
// - the sphere (projection_shaders.h): x right, y up, z towards the centre of
//   an equirectangular frame (longitude 0);
// - the view of the pass: x right, y up, z ahead;
// - OpenXR's view and reference spaces: x right, y up, z behind (-z ahead),
//   right handed. S = diag(1, 1, -1) takes either to the other, so an eye
//   whose orientation is R (view space into the LOCAL space) sees the sphere
//   through S R S, then the turn of the recentering about y.

#ifndef PROJECTION_VIEW_H_
#define PROJECTION_VIEW_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

// The view: yaw and pitch in degrees (yaw positive to the right of the
// frame's centre, pitch positive up), the vertical field of view in degrees,
// and whether it rests (the sharper filter) or moves (bilinear).
//
// Immuch360, an eye of a headset (patch 15): with |eye| true, |rows| (the
// rotation of the view into the sphere, row major) and |tangents| replace yaw,
// pitch and fov.
struct ProjectionView {
  float yaw = 0.0f;
  float pitch = 0.0f;
  float fov = 90.0f;
  bool sharp = true;
  bool eye = false;
  float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  // tan of the left, right, up and down angles, signed as XrFovf's: left and
  // down negative for a view that looks between them
  float tangents[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
};

namespace projection_view {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;

// What the pass gets for a view on an output of a given size.
struct Uniforms {
  // uViewToSphere, column major as glUniformMatrix3fv reads it
  float columns[9];
  // uTanFov: left, right, up, down
  float tangents[4];
  // Output pixels per radian at the centre of the view, vertically: what
  // uTexelsPerPixel divides
  double pixels_per_radian;
};

// Rows of Ry(yaw) * Rx(pitch): the centre of the view looks at longitude
// |yaw| and latitude |pitch|, as the photo sphere does (degrees).
inline void YawPitchRows(double yaw_degrees, double pitch_degrees,
                         double rows[9]) {
  const double yaw = yaw_degrees * kDegree;
  const double pitch = std::clamp(pitch_degrees, -90.0, 90.0) * kDegree;
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  const double cp = std::cos(pitch), sp = std::sin(pitch);
  const double out[9] = {
      cy, -sy * sp, sy * cp,  //
      0,  cp,       sp,       //
      -sy, -cy * sp, cy * cp,
  };
  for (int i = 0; i < 9; i++) rows[i] = out[i];
}

// The four tangents of a symmetric view |fov_degrees| high on an output of
// |aspect| (width / height).
inline void SymmetricTangents(double fov_degrees, double aspect,
                              double out[4]) {
  const double fov = std::clamp(fov_degrees, 1.0, 170.0) * kDegree;
  const double tan_half = std::tan(fov / 2.0);
  out[0] = -tan_half * aspect;
  out[1] = tan_half * aspect;
  out[2] = tan_half;
  out[3] = -tan_half;
}

// The uniforms of |view| on an output |width| x |height| pixels.
inline Uniforms UniformsOf(const ProjectionView& view, int32_t width,
                           int32_t height) {
  Uniforms u{};
  double rows[9];
  double tangents[4];
  if (view.eye) {
    for (int i = 0; i < 9; i++) rows[i] = view.rows[i];
    for (int i = 0; i < 4; i++) tangents[i] = view.tangents[i];
  } else {
    YawPitchRows(view.yaw, view.pitch, rows);
    SymmetricTangents(view.fov,
                      static_cast<double>(width) / (std::max)(1, height),
                      tangents);
  }
  for (int i = 0; i < 9; i++) {
    u.columns[i] = static_cast<float>(rows[(i % 3) * 3 + i / 3]);
  }
  for (int i = 0; i < 4; i++) u.tangents[i] = static_cast<float>(tangents[i]);
  // Height in pixels over the height in tangent units: near the centre a
  // tangent unit is a radian
  const double span = (std::max)(1e-6, tangents[2] - tangents[3]);
  u.pixels_per_radian = (std::max)(1, height) / span;
  return u;
}

// A unit quaternion, OpenXR's order of XrQuaternionf.
struct Quaternion {
  double x = 0, y = 0, z = 0, w = 1;
};

// The rotation of |q|, row major (OpenXR's convention: it takes a direction of
// the view space into the reference space).
inline void QuaternionRows(Quaternion q, double rows[9]) {
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-9) {
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; i++) rows[i] = identity[i];
    return;
  }
  const double x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;
  const double out[9] = {
      1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
      2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
      2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y),
  };
  for (int i = 0; i < 9; i++) rows[i] = out[i];
}

// Where an eye looks, in the sphere's frame before any recentering: the yaw
// in degrees (positive to the right), from the orientation of the eye.
inline double HeadYawDegrees(Quaternion orientation) {
  double r[9];
  QuaternionRows(orientation, r);
  // The forward direction (0, 0, -1) of the view space, in the reference
  // space, then through S: (-r[2], -r[5], r[8])
  const double x = -r[2];
  const double z = r[8];
  if (std::abs(x) < 1e-9 && std::abs(z) < 1e-9) {
    return 0.0;  // straight up or down: no yaw to keep
  }
  return std::atan2(x, z) / kDegree;
}

// The view of an eye: its orientation and its four angles (radians, as
// XrFovf), the sphere turned by |yaw_offset_degrees| about the vertical (the
// recentering, plus the yaw the window had), into |view|. |view|'s sharp flag
// is left as it is.
inline void EyeView(Quaternion orientation, double angle_left,
                    double angle_right, double angle_up, double angle_down,
                    double yaw_offset_degrees, ProjectionView* view) {
  double r[9];
  QuaternionRows(orientation, r);
  // S R S: the entries of the third row and column change sign, the corner
  // twice
  double srs[9];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      const double sign = ((i == 2) != (j == 2)) ? -1.0 : 1.0;
      srs[i * 3 + j] = sign * r[i * 3 + j];
    }
  }
  double turn[9];
  YawPitchRows(yaw_offset_degrees, 0.0, turn);
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      double sum = 0;
      for (int k = 0; k < 3; k++) sum += turn[i * 3 + k] * srs[k * 3 + j];
      view->rows[i * 3 + j] = static_cast<float>(sum);
    }
  }
  view->tangents[0] = static_cast<float>(std::tan(angle_left));
  view->tangents[1] = static_cast<float>(std::tan(angle_right));
  view->tangents[2] = static_cast<float>(std::tan(angle_up));
  view->tangents[3] = static_cast<float>(std::tan(angle_down));
  view->eye = true;
}

}  // namespace projection_view

#endif  // PROJECTION_VIEW_H_
