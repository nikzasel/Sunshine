/**
 * @file src/platform/linux/misc.h
 * @brief Miscellaneous declarations for Linux.
 */
#pragma once

// standard includes
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

// local includes
#include "src/utility.h"

#ifndef DOXYGEN
KITTY_USING_MOVE_T(file_t, int, -1, {
  if (el >= 0) {
    close(el);
  }
});
#else
/**
 * @brief Move-only wrapper for a POSIX file descriptor.
 */
class file_t;
#endif

/**
 * @brief Enumerates supported window system options.
 */
enum class window_system_e {
  NONE,  ///< No window system
  X11,  ///< X11
  WAYLAND,  ///< Wayland
};

extern window_system_e window_system;  ///< Window system.

namespace dyn {
  /**
   * @brief Opaque shared-library handle returned by the dynamic loader.
   */
  struct library_t;

  /**
   * @brief Generic GLX procedure pointer returned by the loader.
   */
  using apiproc = void (*)(void);

  /**
   * @brief Load named functions from an open shared library.
   * @param handle Library whose symbols are queried.
   * @param funcs Function pointer destinations and corresponding symbol names.
   * @param strict Whether missing symbols make the operation fail.
   * @return Zero on success or minus one when a required symbol is missing.
   */
  int load(library_t *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  /**
   * @brief Open the first available shared library from a nonempty list.
   * @return Open library handle, or null if none could be loaded.
   */
  library_t *handle(const std::vector<const char *> &libs);

}  // namespace dyn

#ifdef SUNSHINE_BUILD_PORTAL
namespace portal {
  /**
   * @brief Check if a Portal restore token already exists on disk.
   *
   * @return True if a saved token was found.
   */
  bool has_saved_token();

  /**
   * @brief Clear a restore token if it already exists on disk.
   */
  void clear_saved_token();

  /**
   * @brief Get path of the XDG Portal restore token.
   *
   * @return Path of token that may include the session desktop as a suffix (e.g. .gnome, .kde).
   */
  std::filesystem::path get_saved_token_path();

  #ifdef SUNSHINE_TESTS
  /**
   * @brief Resolve and migrate a saved token using isolated test inputs.
   *
   * @param directory Directory containing test tokens.
   * @param desktop Session desktop used to select the token suffix.
   * @return Path of the selected test token.
   */
  std::filesystem::path get_saved_token_path_for_testing(const std::filesystem::path &directory, const std::string &desktop);
  #endif

  /**
   * @brief Check if the Portal service responds to a DBus Ping within 2 seconds.
   *
   * @return True if the Portal is reachable.
   */
  bool is_portal_service_reachable();

  inline std::jthread xdg_worker;  ///< XDG worker thread to negotiate restore token.
  inline std::once_flag xdg_worker_flag;  ///< Flag to ensure xdg_worker spawns only once.
}  // namespace portal
#endif

#if defined(SUNSHINE_BUILD_NIRI) && defined(SUNSHINE_TESTS)
namespace niri {
  /**
   * @brief An output that unit tests hand to the niri backend instead of a Wayland output.
   */
  struct output_for_testing_t {
    std::string name;  ///< Connector name, or an empty string for an output without one.
    int offset_x = 0;  ///< Logical X position.
    int offset_y = 0;  ///< Logical Y position.
    int width = 0;  ///< Logical width.
    int height = 0;  ///< Logical height.
  };

  /**
   * @brief The stream that the niri backend configured for the shared PipeWire display.
   */
  struct configured_stream_for_testing_t {
    int pipewire_fd = 0;  ///< PipeWire file descriptor, or -1 for the local PipeWire daemon.
    std::uint32_t pipewire_node = 0;  ///< PipeWire node id of the stream.
    std::uint64_t pipewire_object_serial = 0;  ///< PipeWire object serial of the stream.
    int offset_x = 0;  ///< X position of the captured output.
    int offset_y = 0;  ///< Y position of the captured output.
    int width = 0;  ///< Width of the captured output.
    int height = 0;  ///< Height of the captured output.
  };

  /**
   * @brief Talk to org.gnome.Mutter.ScreenCast on the given bus instead of the session bus.
   *
   * @param bus_address Address of the message bus.
   */
  void set_bus_address_for_testing(const std::string &bus_address);

  /**
   * @brief Use the given outputs instead of the outputs of the Wayland compositor.
   *
   * @param outputs Outputs in any order.
   */
  void set_outputs_for_testing(const std::vector<output_for_testing_t> &outputs);

  /**
   * @brief Wait the given time for PipeWireStreamAdded instead of the default timeout.
   *
   * @param timeout Time to wait after Start.
   */
  void set_stream_added_timeout_for_testing(std::chrono::milliseconds timeout);

  /**
   * @brief Restore the session bus, the Wayland outputs and the default timeout.
   */
  void reset_overrides_for_testing();

  /**
   * @brief Configure the stream of a niri display, then stop its session.
   *
   * @param display_name Connector name of the output to record, or an empty string for the first output.
   * @param stream Receives the configured stream.
   * @return 0 on success, -1 on failure.
   */
  int configure_stream_for_testing(const std::string &display_name, configured_stream_for_testing_t &stream);
}  // namespace niri
#endif

namespace platf {
  /**
   * @brief Remove environment variables that can load untrusted GUI modules.
   *
   * Loader paths and module lists used by Qt, GTK, GIO, and GdkPixbuf are removed before any of those libraries
   * can initialize. The caller determines whether privileged execution requires the policy.
   *
   * @return `true` when the environment is safe to use; `false` if a variable could not be removed.
   */
  bool sanitize_process_environment();

  /**
   * @brief Open a DRM card node and drop implicit DRM master, if any.
   *
   * Performs `open(path, flags | O_CLOEXEC)` and probes the resulting fd with
   * `DRM_IOCTL_AUTH_MAGIC`. If the kernel implicitly handed us master, calls
   * `drmDropMaster` and re-verifies before returning. Master check/drop failures
   * are logged as warnings but do not fail the call: the caller still receives
   * a usable fd.
   *
   * Callers should use this helper for any `/dev/dri/cardN` open so we never
   * keep implicit master and block compositors from re-acquiring it on VT
   * switches.
   *
   * @param path Path to the DRM card node (e.g. `/dev/dri/card0`).
   * @param flags `open()` flags. `O_CLOEXEC` is always OR-ed in.
   * @return A file descriptor on success, or `-1` if `open()` itself fails.
   */
  int open_drm_card_fd(const std::filesystem::path &path, int flags = O_RDWR);

  /**
   * @brief Generic frame pacing logic for Linux capture methods.
   *
   * Advances the frame pacing timeline and re-anchors it if the capture thread falls behind.
   *
   * @param next_frame Time point that the next frame will be targeted against.
   * @param delay Delay interval used to pace next frame.
   * @param logger The sleep_overshoot_logger for tracking discontinuities.
   */
  inline void handle_pacing(std::chrono::steady_clock::time_point &next_frame, std::chrono::nanoseconds delay, auto &logger) {
    auto now = std::chrono::steady_clock::now();

    if (next_frame > now) {
      std::this_thread::sleep_until(next_frame);
      logger.first_point(next_frame);
      logger.second_point_now_and_log();
    }

    next_frame += delay;
    if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
      next_frame = now + delay;
    }
  }

#if defined(__linux__) && defined(SUNSHINE_BUILD_DRM)
  namespace kms {
    /**
     * @brief Force-construct the DRM privileged worker thread while the calling
     *        thread still holds CAP_SYS_ADMIN in CAP_PERMITTED.
     *
     * This must be called before dropping CAP_SYS_ADMIN from the calling thread,
     * since newly-created threads inherit its capability sets.
     */
    void ensure_privileged_drm_worker_started();

    /**
     * @brief Drop the DRM worker thread's privileges.
     */
    void drop_drm_worker_privileges();

    /**
     * @brief Open a DRM card file descriptor using the privileged DRM worker.
     *
     * @param path Path to the DRM card node.
     * @return A file descriptor on success, or `-1` on failure.
     */
    int privileged_open_drm_card_fd(const char *path);
  }  // namespace kms
#endif
}  // namespace platf
