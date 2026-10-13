/**
 * @file src/platform/linux/nirigrab.cpp
 * @brief niri direct ScreenCast capture via the org.gnome.Mutter.ScreenCast D-Bus API.
 *
 * Bypasses xdg-desktop-portal entirely. niri serves org.gnome.Mutter.ScreenCast on the
 * session bus and records a monitor without an authorization step, so this backend
 * needs no source-selection dialog and no restore token. It records one monitor by its
 * connector name, starts the session and takes the PipeWire node id from the
 * PipeWireStreamAdded signal. Frames then flow through the shared PipeWire path
 * (pipewire.cpp), the same one the portal backend uses.
 *
 * Chain: niri -> org.gnome.Mutter.ScreenCast (session bus) -> PipeWire -> Sunshine
 *
 * The backend is opt-in: it is selected only by `capture = niri`, never automatically.
 */
// standard includes
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "pipewire.cpp"

using namespace std::literals;

namespace {
  constexpr const char *DBUS_NAME = "org.freedesktop.DBus";
  constexpr const char *DBUS_PATH = "/org/freedesktop/DBus";
  constexpr const char *DBUS_IFACE = "org.freedesktop.DBus";
  constexpr const char *DBUS_PROPERTIES_IFACE = "org.freedesktop.DBus.Properties";

  constexpr const char *MUTTER_SCREENCAST_NAME = "org.gnome.Mutter.ScreenCast";
  constexpr const char *MUTTER_SCREENCAST_PATH = "/org/gnome/Mutter/ScreenCast";
  constexpr const char *MUTTER_SCREENCAST_IFACE = "org.gnome.Mutter.ScreenCast";
  constexpr const char *MUTTER_SESSION_IFACE = "org.gnome.Mutter.ScreenCast.Session";
  constexpr const char *MUTTER_STREAM_IFACE = "org.gnome.Mutter.ScreenCast.Stream";

  // org.gnome.Mutter.ScreenCast cursor-mode values: 0 = hidden, 1 = embedded, 2 = metadata.
  // Embedded matches what the portal backend requests (portal CURSOR_MODE_EMBEDDED).
  constexpr uint32_t MUTTER_CURSOR_MODE_EMBEDDED = 1;

  constexpr int DBUS_CALL_TIMEOUT_MS = 5000;
  constexpr guint STREAM_ADDED_TIMEOUT_MS = 5000;
}  // namespace

namespace niri {
  /**
   * @brief Wait state for the PipeWireStreamAdded signal of one stream.
   *
   * The wait also ends on the Closed signal of the stream's session: niri closes the
   * session instead of adding a stream when it cannot start the cast.
   */
  struct stream_added_t {
    GMainLoop *loop = nullptr;  ///< Loop that runs until a signal or the timeout arrives.
    uint32_t node_id = PW_ID_ANY;  ///< PipeWire node id carried by the signal.
    bool closed = false;  ///< Whether the session closed before the signal arrived.
  };

  /**
   * @brief Stream geometry reported by the compositor in logical coordinates.
   */
  struct stream_parameters_t {
    int pos_x = 0;  ///< Output X position.
    int pos_y = 0;  ///< Output Y position.
    int width = 0;  ///< Output logical width.
    int height = 0;  ///< Output logical height.
  };

  /**
   * @brief Wayland outputs sorted by position, so names and the fallback output are stable.
   *
   * @return Outputs that carry a connector name, left to right, then top to bottom.
   */
  inline std::vector<std::unique_ptr<wl::monitor_t>> sorted_monitors() {
    auto monitors = wl::monitors();
    std::erase_if(monitors, [](const auto &monitor) {
      return monitor->name.empty();
    });
    std::ranges::sort(monitors, [](const auto &a, const auto &b) {
      if (a->viewport.offset_x != b->viewport.offset_x) {
        return a->viewport.offset_x < b->viewport.offset_x;
      }
      return a->viewport.offset_y < b->viewport.offset_y;
    });
    return monitors;
  }

  /**
   * @brief Connect to the session bus.
   *
   * @return Session bus connection, or nullptr on failure.
   */
  inline GDBusConnection *session_bus() {
    g_autoptr(GError) err = nullptr;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn) {
      BOOST_LOG(error) << "[nirigrab] Could not connect to the session bus: "sv << err->message;
    }
    return conn;
  }

  /**
   * @brief Resolve the unique bus name that owns org.gnome.Mutter.ScreenCast.
   *
   * @param conn Bus connection to query.
   * @return Unique bus name, or an empty string when nobody owns the name.
   */
  inline std::string service_owner(GDBusConnection *conn) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      conn,
      DBUS_NAME,
      DBUS_PATH,
      DBUS_IFACE,
      "GetNameOwner",
      g_variant_new("(s)", MUTTER_SCREENCAST_NAME),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_CALL_TIMEOUT_MS,
      nullptr,
      &err
    );
    if (!reply) {
      BOOST_LOG(debug) << "[nirigrab] "sv << MUTTER_SCREENCAST_NAME << " has no owner: "sv << err->message;
      return {};
    }
    const gchar *owner = nullptr;
    g_variant_get(reply, "(&s)", &owner);
    return owner;
  }

  /**
   * @brief One org.gnome.Mutter.ScreenCast session recording a single monitor.
   *
   * The destructor stops the session. niri does not stop a session when its D-Bus
   * client goes away, so the explicit Stop is what releases the compositor's stream.
   */
  class screencast_t {
  public:
    /**
     * @brief Create a session handle on a bus connection.
     *
     * @param connection Connection to the bus that serves org.gnome.Mutter.ScreenCast. The handle takes ownership.
     */
    explicit screencast_t(GDBusConnection *connection):
        conn {connection} {
    }

    screencast_t(const screencast_t &) = delete;
    screencast_t &operator=(const screencast_t &) = delete;

    ~screencast_t() {
      stop();
      g_clear_object(&conn);
    }

    /**
     * @brief Record a monitor and wait until its PipeWire stream exists.
     *
     * CreateSession -> RecordMonitor(connector, cursor-mode) -> Start -> PipeWireStreamAdded(node_id).
     *
     * @param connector Connector name of the monitor, as niri names its outputs.
     * @return 0 on success, -1 on failure. On success node_id and parameters are populated.
     */
    int start(const std::string &connector) {
      owner = service_owner(conn);
      if (owner.empty()) {
        BOOST_LOG(error) << "[nirigrab] "sv << MUTTER_SCREENCAST_NAME << " is not on the session bus"sv;
        return -1;
      }

      if (create_session() < 0 || record_monitor(connector) < 0) {
        return -1;
      }
      read_stream_parameters();

      if (start_and_wait_for_node() < 0) {
        return -1;
      }

      BOOST_LOG(info) << "[nirigrab] Pipewire stream created: node="sv << node_id
                      << " connector "sv << connector
                      << " position "sv << parameters.pos_x << "x"sv << parameters.pos_y
                      << " resolution "sv << parameters.width << "x"sv << parameters.height;
      return 0;
    }

    /**
     * @brief Stop the session, if one was created. Safe to call more than once.
     */
    void stop() {
      if (session_path.empty()) {
        return;
      }
      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        conn,
        MUTTER_SCREENCAST_NAME,
        session_path.c_str(),
        MUTTER_SESSION_IFACE,
        "Stop",
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &err
      );
      if (err) {
        // The compositor may already have stopped the session (e.g. its output went away).
        BOOST_LOG(debug) << "[nirigrab] Stop of "sv << session_path << " failed: "sv << err->message;
      } else {
        BOOST_LOG(debug) << "[nirigrab] Stopped session "sv << session_path;
      }
      session_path.clear();
      stream_path.clear();
    }

    uint32_t node_id = PW_ID_ANY;  ///< PipeWire node id of the recorded monitor.
    stream_parameters_t parameters;  ///< Stream geometry (logical), when the compositor reports it.
    bool has_parameters = false;  ///< Whether parameters came from the compositor.

  private:
    GDBusConnection *conn = nullptr;
    std::string owner;
    std::string session_path;
    std::string stream_path;

    int create_session() {
      GVariantBuilder properties;
      g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        conn,
        MUTTER_SCREENCAST_NAME,
        MUTTER_SCREENCAST_PATH,
        MUTTER_SCREENCAST_IFACE,
        "CreateSession",
        g_variant_new("(a{sv})", &properties),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &err
      );
      if (!reply) {
        BOOST_LOG(error) << "[nirigrab] CreateSession failed: "sv << err->message;
        return -1;
      }
      const gchar *path = nullptr;
      g_variant_get(reply, "(&o)", &path);
      session_path = path;
      BOOST_LOG(debug) << "[nirigrab] Created session "sv << session_path;
      return 0;
    }

    int record_monitor(const std::string &connector) {
      GVariantBuilder properties;
      g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&properties, "{sv}", "cursor-mode", g_variant_new_uint32(MUTTER_CURSOR_MODE_EMBEDDED));

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        conn,
        MUTTER_SCREENCAST_NAME,
        session_path.c_str(),
        MUTTER_SESSION_IFACE,
        "RecordMonitor",
        g_variant_new("(sa{sv})", connector.c_str(), &properties),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &err
      );
      if (!reply) {
        BOOST_LOG(error) << "[nirigrab] RecordMonitor('"sv << connector << "') failed: "sv << err->message;
        return -1;
      }
      const gchar *path = nullptr;
      g_variant_get(reply, "(&o)", &path);
      stream_path = path;
      BOOST_LOG(debug) << "[nirigrab] Recording monitor '"sv << connector << "' on stream "sv << stream_path;
      return 0;
    }

    void read_stream_parameters() {
      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        conn,
        MUTTER_SCREENCAST_NAME,
        stream_path.c_str(),
        DBUS_PROPERTIES_IFACE,
        "Get",
        g_variant_new("(ss)", MUTTER_STREAM_IFACE, "Parameters"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &err
      );
      if (!reply) {
        BOOST_LOG(warning) << "[nirigrab] Could not read stream Parameters: "sv << err->message;
        return;
      }
      g_autoptr(GVariant) value = nullptr;
      g_variant_get(reply, "(v)", &value);
      if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT)) {
        BOOST_LOG(warning) << "[nirigrab] Stream Parameters is not a dictionary"sv;
        return;
      }
      stream_parameters_t params;
      const bool has_position = g_variant_lookup(value, "position", "(ii)", &params.pos_x, &params.pos_y);
      const bool has_size = g_variant_lookup(value, "size", "(ii)", &params.width, &params.height);
      if (has_position && has_size && params.width > 0 && params.height > 0) {
        parameters = params;
        has_parameters = true;
      }
    }

    int start_and_wait_for_node() {
      // Subscribe before Start: niri emits PipeWireStreamAdded as soon as its stream is created.
      // The signal is dispatched on a private context so no other GLib loop can steal it.
      GMainContext *context = g_main_context_new();
      g_main_context_push_thread_default(context);
      GMainLoop *loop = g_main_loop_new(context, FALSE);

      stream_added_t state;
      state.loop = loop;
      const guint subscription = g_dbus_connection_signal_subscribe(
        conn,
        owner.c_str(),
        MUTTER_STREAM_IFACE,
        "PipeWireStreamAdded",
        stream_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_stream_added,
        &state,
        nullptr
      );
      // A cast that niri rejects (no PipeWire, no GBM device) ends in Session.Closed, never
      // in PipeWireStreamAdded. Without this subscription each rejected cast waits out
      // STREAM_ADDED_TIMEOUT_MS, once per encoder-probe display.
      const guint closed_subscription = g_dbus_connection_signal_subscribe(
        conn,
        owner.c_str(),
        MUTTER_SESSION_IFACE,
        "Closed",
        session_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_session_closed,
        &state,
        nullptr
      );

      int result = 0;
      {
        g_autoptr(GError) err = nullptr;
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
          conn,
          MUTTER_SCREENCAST_NAME,
          session_path.c_str(),
          MUTTER_SESSION_IFACE,
          "Start",
          nullptr,
          nullptr,
          G_DBUS_CALL_FLAGS_NONE,
          DBUS_CALL_TIMEOUT_MS,
          nullptr,
          &err
        );
        if (err) {
          BOOST_LOG(error) << "[nirigrab] Start failed: "sv << err->message;
          result = -1;
        }
      }

      if (result == 0) {
        GSource *timeout = g_timeout_source_new(STREAM_ADDED_TIMEOUT_MS);
        g_source_set_callback(timeout, on_stream_added_timeout, &state, nullptr);
        g_source_attach(timeout, context);
        g_main_loop_run(loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);

        if (state.node_id != PW_ID_ANY) {
          node_id = state.node_id;
        } else if (state.closed) {
          BOOST_LOG(error) << "[nirigrab] Compositor closed session "sv << session_path << " before PipeWireStreamAdded on "sv << stream_path;
          result = -1;
        } else {
          BOOST_LOG(error) << "[nirigrab] No PipeWire node id for "sv << stream_path << ": no PipeWireStreamAdded within "sv << STREAM_ADDED_TIMEOUT_MS << " ms, or one with an unexpected signature"sv;
          result = -1;
        }
      }

      g_dbus_connection_signal_unsubscribe(conn, subscription);
      g_dbus_connection_signal_unsubscribe(conn, closed_subscription);
      // Drain anything already queued for the subscription before its state goes out of scope.
      while (g_main_context_iteration(context, FALSE)) {
      }
      g_main_loop_unref(loop);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      return result;
    }

    static void on_stream_added([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender_name, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *signal_name, GVariant *signal_parameters, gpointer user_data) {
      auto *state = static_cast<stream_added_t *>(user_data);
      if (g_variant_is_of_type(signal_parameters, G_VARIANT_TYPE("(u)"))) {
        g_variant_get(signal_parameters, "(u)", &state->node_id);
      } else {
        BOOST_LOG(error) << "[nirigrab] PipeWireStreamAdded with unexpected signature: "sv << g_variant_get_type_string(signal_parameters);
      }
      g_main_loop_quit(state->loop);
    }

    static void on_session_closed([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender_name, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *signal_name, [[maybe_unused]] GVariant *signal_parameters, gpointer user_data) {
      auto *state = static_cast<stream_added_t *>(user_data);
      state->closed = true;
      g_main_loop_quit(state->loop);
    }

    static gboolean on_stream_added_timeout(gpointer user_data) {
      auto *state = static_cast<stream_added_t *>(user_data);
      g_main_loop_quit(state->loop);
      return G_SOURCE_REMOVE;
    }
  };

  /**
   * @brief niri screencast display backend on top of the shared PipeWire display.
   */
  class niri_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      const auto monitors = sorted_monitors();
      if (monitors.empty()) {
        BOOST_LOG(error) << "[nirigrab] No Wayland output with a connector name found"sv;
        return -1;
      }

      // Exact connector match, else the first output (left-most, then top-most).
      const wl::monitor_t *monitor = monitors.front().get();
      if (!display_name.empty()) {
        const auto it = std::ranges::find_if(monitors, [&](const auto &m) {
          return m->name == display_name;
        });
        if (it != monitors.end()) {
          monitor = it->get();
        } else {
          BOOST_LOG(info) << "[nirigrab] No output named '"sv << display_name << "', using '"sv << monitor->name << "'"sv;
        }
      }

      GDBusConnection *bus = session_bus();
      if (!bus) {
        return -1;
      }
      screencast = std::make_unique<screencast_t>(bus);
      if (screencast->start(monitor->name) < 0) {
        return -1;
      }

      // Return values for pipewire init
      out_pipewire_fd = -1;  // niri's stream lives on the local PipeWire daemon
      out_pipewire_node = screencast->node_id;
      out_pipewire_objectserial = SPA_ID_INVALID;  // Mutter ScreenCast reports a node id only
      // Set/update basic stream parameters on display_t
      if (screencast->has_parameters) {
        this->offset_x = screencast->parameters.pos_x;
        this->offset_y = screencast->parameters.pos_y;
        this->width = screencast->parameters.width;
        this->height = screencast->parameters.height;
      } else {
        this->offset_x = monitor->viewport.offset_x;
        this->offset_y = monitor->viewport.offset_y;
        this->width = monitor->viewport.logical_width;
        this->height = monitor->viewport.logical_height;
      }
      this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
      this->logical_height = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
      return 0;
    }

    std::unique_ptr<screencast_t> screencast;  ///< Active org.gnome.Mutter.ScreenCast session.
  };
}  // namespace niri

// Public API for misc.cpp
namespace platf {
  /**
   * @brief Create a niri screencast display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name (niri connector name, e.g. DP-1).
   * @param config Configuration values to apply.
   * @return niri/PipeWire display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> niri_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[nirigrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<niri::niri_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate the outputs niri can record.
   *
   * @return Connector names, or an empty list when no output is known.
   */
  std::vector<std::string> niri_display_names() {
    std::vector<std::string> display_names;
    for (const auto &monitor : niri::sorted_monitors()) {
      BOOST_LOG(info) << "[nirigrab] Found output: "sv << monitor->name << " position: "sv << monitor->viewport.offset_x << "x"sv << monitor->viewport.offset_y << " resolution: "sv << monitor->viewport.logical_width << "x"sv << monitor->viewport.logical_height;
      display_names.emplace_back(monitor->name);
    }
    return display_names;
  }

  /**
   * @brief Check whether org.gnome.Mutter.ScreenCast is served on the session bus.
   *
   * @return True when the ScreenCast service has an owner.
   */
  bool niri_available() {
    g_autoptr(GDBusConnection) conn = niri::session_bus();
    if (!conn) {
      return false;
    }
    if (niri::service_owner(conn).empty()) {
      BOOST_LOG(error) << "[nirigrab] capture = niri, but "sv << MUTTER_SCREENCAST_NAME << " is not on the session bus"sv;
      return false;
    }
    return true;
  }
}  // namespace platf
