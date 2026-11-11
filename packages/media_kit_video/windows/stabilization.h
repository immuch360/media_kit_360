// Immuch360: the stabilisation of a raw 360 video by renderer C
// (IMMUCH360-NOTE.md, patch 14).
//
// The app reads the motion track of the camera (Insta360 gyroscope, GoPro
// CORI and GRAV, DJI attitude) and gives one correction per frame, keyed on
// the presentation time of the frame (lib/domain/models/raw/stabilization.dart
// of the app). A point of the stitched sphere at direction d of the stitch's
// view frame (x right, y down, z forward) is shown at direction K d, K the
// rotation of the frame's correction quaternion (w, x, y, z). So the pass
// samples the stitch at K^T v for each output direction v
// (projection_shaders.h, uCorrection), the convention of the Android player
// (StabilizationTable.kt of the app).
//
// Which frame is drawn: the render API of libmpv gives no time with the frame
// it draws. mpv sets time-pos to the time of a frame when it hands that frame
// over (with video-timing-offset 0, at its display time), so time-pos read
// when mpv says it has a frame is that frame's time, unless the draw comes
// after the display time of the next frame, which mpv has then handed over:
// one frame late. |FrameClock| tells the two cases apart with the target time
// of the frame (MPV_RENDER_PARAM_NEXT_FRAME_INFO), measured on 2026-11-11
// with frames carrying their index (36 runs of 12 s on Windows, the libmpv
// and ANGLE of the app, Intel UHD; 36 on Linux, mpv 0.35): 3.35% of the draws
// one frame off with time-pos alone, 0.15% with the rule, all of them among
// the first five draws of a file.
//
// No Windows header here: windows/test/stabilization_test.cc builds it with
// any C++17 compiler.

#ifndef STABILIZATION_H_
#define STABILIZATION_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace stabilization {

// The rotation blended between |a| and |b| at |share| (0 at a, 1 at b), along
// the shorter arc, unit long; quaternions (w, x, y, z)
inline void Slerp(const double a[4],
                  const double b[4],
                  double share,
                  double out[4]) {
  double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const double sign = dot < 0.0 ? -1.0 : 1.0;
  dot *= sign;
  double wa;
  double wb;
  if (dot > 1.0 - 1e-10) {
    wa = 1.0 - share;
    wb = share;
  } else {
    const double angle = std::acos((std::min)(1.0, dot));
    const double sine = std::sin(angle);
    wa = std::sin((1.0 - share) * angle) / sine;
    wb = std::sin(share * angle) / sine;
  }
  wb *= sign;
  double norm = 0.0;
  for (int i = 0; i < 4; i++) {
    out[i] = wa * a[i] + wb * b[i];
    norm += out[i] * out[i];
  }
  norm = std::sqrt(norm);
  for (int i = 0; i < 4; i++) {
    out[i] /= norm;
  }
}

// The rotation matrix of the unit quaternion |q| (w, x, y, z), acting on
// vectors, row major
inline void MatrixOf(const double q[4], double rows[9]) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  rows[0] = 1 - 2 * (y * y + z * z);
  rows[1] = 2 * (x * y - w * z);
  rows[2] = 2 * (x * z + w * y);
  rows[3] = 2 * (x * y + w * z);
  rows[4] = 1 - 2 * (x * x + z * z);
  rows[5] = 2 * (y * z - w * x);
  rows[6] = 2 * (x * z - w * y);
  rows[7] = 2 * (y * z + w * x);
  rows[8] = 1 - 2 * (x * x + y * y);
}

// The corrections of a video, one per frame, as the app gives them
class Table {
 public:
  // The table of |times_us| (rising) and |corrections| (four values a frame,
  // unit quaternions), or null with |reason| for one that cannot be used
  static std::shared_ptr<const Table> Of(std::vector<int64_t> times_us,
                                         std::vector<double> corrections,
                                         std::string* reason) {
    const auto refuse = [reason](std::string why) {
      if (reason != nullptr) {
        *reason = std::move(why);
      }
      return std::shared_ptr<const Table>();
    };
    if (times_us.empty()) {
      return refuse("no frame");
    }
    if (corrections.size() != 4 * times_us.size()) {
      return refuse(std::to_string(corrections.size()) + " values for " +
                    std::to_string(times_us.size()) + " frames");
    }
    for (size_t i = 1; i < times_us.size(); i++) {
      if (times_us[i] <= times_us[i - 1]) {
        return refuse("frame " + std::to_string(i) + " does not follow " +
                      "the one before it");
      }
    }
    for (size_t i = 0; i < times_us.size(); i++) {
      const double* q = &corrections[4 * i];
      const double norm =
          std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-3) {
        return refuse("frame " + std::to_string(i) +
                      " has a correction that is no rotation");
      }
    }
    return std::shared_ptr<const Table>(
        new Table(std::move(times_us), std::move(corrections)));
  }

  size_t size() const { return times_us_.size(); }

  // The correction at |time_us| of the timeline: the one of the frame at
  // that time, blended (slerp) between the frames around it, the first or the
  // last one outside them
  void QuaternionAt(int64_t time_us, double out[4]) const {
    const auto& times = times_us_;
    if (time_us <= times.front()) {
      Copy(0, out);
      return;
    }
    if (time_us >= times.back()) {
      Copy(times.size() - 1, out);
      return;
    }
    // The last frame at or before time_us
    const auto after = std::upper_bound(times.begin(), times.end(), time_us);
    const size_t high = static_cast<size_t>(after - times.begin());
    const size_t low = high - 1;
    const double share = static_cast<double>(time_us - times[low]) /
                         static_cast<double>(times[high] - times[low]);
    Slerp(&corrections_[4 * low], &corrections_[4 * high], share, out);
  }

  // What the pass takes as uCorrection at |time_us|: K^T, uploaded column
  // major, which is K row major
  void SampleMatrixAt(int64_t time_us, float columns[9]) const {
    double q[4];
    QuaternionAt(time_us, q);
    double rows[9];
    MatrixOf(q, rows);
    for (int i = 0; i < 9; i++) {
      columns[i] = static_cast<float>(rows[i]);
    }
  }

 private:
  Table(std::vector<int64_t> times_us, std::vector<double> corrections)
      : times_us_(std::move(times_us)), corrections_(std::move(corrections)) {}

  void Copy(size_t frame, double out[4]) const {
    for (int i = 0; i < 4; i++) {
      out[i] = corrections_[4 * frame + i];
    }
  }

  std::vector<int64_t> times_us_;
  std::vector<double> corrections_;
};

// The time of the frame mpv hands over, from what can be read when mpv says
// it has one (see the top of this file)
class FrameClock {
 public:
  // |time_pos|, mpv's time-pos in seconds read right before the frame is
  // drawn; |target| the frame's target time (mpv_render_frame_info), 0 when
  // mpv gives none; |now| mpv_get_time_us() read with it; |speed| the
  // playback speed; |period| the file's frame duration in seconds. The
  // target time is in microseconds in the API's words, in nanoseconds in mpv
  // 0.41: a target far beyond |now| is taken for nanoseconds.
  double FrameTime(double time_pos,
                   int64_t target,
                   int64_t now,
                   double speed,
                   double period) {
    if (target <= 0 || now <= 0 || !(period > 0.0)) {
      return time_pos;
    }
    if (target > 50 * now) {
      target /= 1000;
    }
    if (!(speed > 0.0)) {
      speed = 1.0;
    }
    const double late = static_cast<double>(now - target) / 1e6;
    if (late < 0.75 * period / speed) {
      // In time: the next frame was not handed over yet
      has_reference_ = true;
      reference_target_ = target;
      reference_time_ = time_pos;
      return time_pos;
    }
    if (!has_reference_ ||
        std::abs(static_cast<double>(target - reference_target_)) > 2e6) {
      return time_pos;
    }
    // Late: time-pos is this frame's or the next one's, whichever the
    // timeline of the last frame drawn in time puts nearer to this frame's
    // target time; neither near it (a seek), time-pos
    const double expected =
        reference_time_ +
        static_cast<double>(target - reference_target_) / 1e6 * speed;
    if (std::abs(expected - time_pos) > 2.0 * period) {
      return time_pos;
    }
    const double previous = time_pos - period;
    return std::abs(previous - expected) < std::abs(time_pos - expected)
               ? previous
               : time_pos;
  }

  void Reset() { has_reference_ = false; }

 private:
  bool has_reference_ = false;
  int64_t reference_target_ = 0;
  double reference_time_ = 0.0;
};

}  // namespace stabilization

#endif  // STABILIZATION_H_
