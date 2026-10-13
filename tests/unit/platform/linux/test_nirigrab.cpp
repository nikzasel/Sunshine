/**
 * @file tests/unit/platform/linux/test_nirigrab.cpp
 * @brief Test the niri capture backend against a fake org.gnome.Mutter.ScreenCast service.
 */
#ifdef SUNSHINE_BUILD_NIRI
  // standard includes
  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <future>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <utility>
  #include <vector>

  // lib includes
  #include <fcntl.h>
  #include <gio/gio.h>
  #include <sys/stat.h>
  #include <unistd.h>

  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/logging.h"
  #include "src/platform/linux/misc.h"

using namespace std::literals;

namespace platf {
  bool niri_available();
  std::vector<std::string> niri_display_names();
}  // namespace platf

namespace {
  constexpr const char *SCREENCAST_NAME = "org.gnome.Mutter.ScreenCast";
  constexpr const char *SCREENCAST_PATH = "/org/gnome/Mutter/ScreenCast";
  constexpr const char *SESSION_PATH = "/org/gnome/Mutter/ScreenCast/Session/u1";
  constexpr const char *STREAM_PATH = "/org/gnome/Mutter/ScreenCast/Stream/u1";
  constexpr const char *SESSION_IFACE = "org.gnome.Mutter.ScreenCast.Session";
  constexpr const char *STREAM_IFACE = "org.gnome.Mutter.ScreenCast.Stream";
  constexpr std::uint32_t NODE_ID = 42;
  constexpr std::uint32_t SPA_ID_INVALID = 0xffffffff;

  /**
   * @brief The output that every test records unless it sets its own.
   */
  const niri::output_for_testing_t DP_1 {"DP-1", 100, 50, 1920, 1080};

  /**
   * @brief The stream geometry that the fake service reports in Parameters.
   */
  constexpr niri::configured_stream_for_testing_t REPORTED_GEOMETRY {.offset_x = 10, .offset_y = 20, .width = 1280, .height = 720};

  /**
   * @brief Environment variables that g_test_dbus_up() unsets or replaces.
   */
  constexpr std::array GTEST_DBUS_ENVIRONMENT {"DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "DISPLAY", "XDG_RUNTIME_DIR"};

  /**
   * @brief The part of the org.gnome.Mutter.ScreenCast API that the backend uses.
   */
  constexpr const char *SCREENCAST_XML = R"(<node>
  <interface name="org.gnome.Mutter.ScreenCast">
    <method name="CreateSession">
      <arg name="properties" type="a{sv}" direction="in"/>
      <arg name="session_path" type="o" direction="out"/>
    </method>
  </interface>
  <interface name="org.gnome.Mutter.ScreenCast.Session">
    <method name="Start"/>
    <method name="Stop"/>
    <method name="RecordMonitor">
      <arg name="connector" type="s" direction="in"/>
      <arg name="properties" type="a{sv}" direction="in"/>
      <arg name="stream_path" type="o" direction="out"/>
    </method>
    <signal name="Closed"/>
  </interface>
  <interface name="org.gnome.Mutter.ScreenCast.Stream">
    <signal name="PipeWireStreamAdded">
      <arg name="node_id" type="u"/>
    </signal>
    <property name="Parameters" type="a{sv}" access="read"/>
  </interface>
</node>)";

  /**
   * @brief What the fake service does after it answers Start.
   */
  enum class start_outcome_e {
    stream_added,  ///< Emit PipeWireStreamAdded with a node id.
    stream_added_wrong_signature,  ///< Emit PipeWireStreamAdded with a string instead of a node id.
    session_closed,  ///< Emit Session.Closed instead of PipeWireStreamAdded, as niri does when it cannot start a cast.
    nothing,  ///< Emit no signal.
  };

  /**
   * @brief What the fake service answers when the stream's Parameters property is read.
   */
  enum class parameters_e {
    complete,  ///< Position and size.
    incomplete,  ///< Position only.
    zero_width,  ///< Position and a size with a width of 0.
    zero_height,  ///< Position and a size with a height of 0.
    failing,  ///< A D-Bus error.
  };

  /**
   * @brief Fake org.gnome.Mutter.ScreenCast service with its own connection and main loop thread.
   */
  class fake_screencast_t {
  public:
    /**
     * @brief Connect to the bus, export the service and own its name.
     *
     * @param bus_address Address of the message bus to serve on.
     * @param outcome What to do after Start.
     * @param failing_method Method that answers with a D-Bus error, or an empty string.
     * @param parameters What reading the stream's Parameters property returns.
     */
    fake_screencast_t(const std::string &bus_address, start_outcome_e outcome, std::string failing_method = {}, parameters_e parameters = parameters_e::complete):
        outcome_ {outcome},
        failing_method_ {std::move(failing_method)},
        parameters_ {parameters} {
      std::promise<bool> exported;
      auto exported_future = exported.get_future();
      thread_ = std::jthread([this, bus_address, &exported]() {
        run(bus_address, exported);
      });
      ready_ = exported_future.get();
    }

    fake_screencast_t(const fake_screencast_t &) = delete;
    fake_screencast_t &operator=(const fake_screencast_t &) = delete;

    ~fake_screencast_t() {
      shutdown();
      g_main_loop_unref(loop_);
    }

    /**
     * @brief Whether the service owns its name and serves calls.
     *
     * @return True when the service is ready.
     */
    bool ready() const {
      return ready_;
    }

    /**
     * @brief Stop serving. The recorded calls are safe to read afterwards.
     */
    void shutdown() {
      if (thread_.joinable()) {
        if (ready_) {
          // Quit from inside the loop. A g_main_loop_quit() from this thread could arrive before the
          // service thread enters g_main_loop_run(), which would then never return.
          g_main_context_invoke(
            context_,
            [](gpointer loop) {
              g_main_loop_quit(static_cast<GMainLoop *>(loop));
              return G_SOURCE_REMOVE;
            },
            loop_
          );
        }
        thread_.join();
      }
    }

    std::string connector;  ///< Connector passed to RecordMonitor.
    std::optional<std::uint32_t> cursor_mode;  ///< cursor-mode passed to RecordMonitor.
    int create_session_calls = 0;  ///< Number of CreateSession calls.
    int stop_calls = 0;  ///< Number of Stop calls, including failed ones.

  private:
    void run(const std::string &bus_address, std::promise<bool> &exported) {
      GMainContext *context = g_main_context_new();
      g_main_context_push_thread_default(context);
      context_ = context;
      loop_ = g_main_loop_new(context, FALSE);

      const bool serving = export_service(bus_address);
      exported.set_value(serving);
      if (serving) {
        g_main_loop_run(loop_);
        // The bus is shared by all tests: give the name back before the next test asks for it.
        if (GVariant *released = g_dbus_connection_call_sync(conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", SCREENCAST_NAME), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr)) {
          g_variant_unref(released);
        }
      }

      for (const auto id : registrations_) {
        g_dbus_connection_unregister_object(conn_, id);
      }
      if (conn_) {
        g_dbus_connection_close_sync(conn_, nullptr, nullptr);
        g_clear_object(&conn_);
      }
      while (g_main_context_iteration(context, FALSE)) {
      }
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
    }

    bool export_service(const std::string &bus_address) {
      g_autoptr(GError) err = nullptr;
      conn_ = g_dbus_connection_new_for_address_sync(
        bus_address.c_str(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        nullptr,
        &err
      );
      if (!conn_) {
        return false;
      }

      g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(SCREENCAST_XML, &err);
      if (!node) {
        return false;
      }
      static const GDBusInterfaceVTable vtable {handle_method_call, handle_get_property, nullptr, {}};
      for (const auto &[path, iface] : {std::pair {SCREENCAST_PATH, SCREENCAST_NAME}, std::pair {SESSION_PATH, SESSION_IFACE}, std::pair {STREAM_PATH, STREAM_IFACE}}) {
        const auto id = g_dbus_connection_register_object(conn_, path, g_dbus_node_info_lookup_interface(node, iface), &vtable, this, nullptr, &err);
        if (id == 0) {
          return false;
        }
        registrations_.push_back(id);
      }

      constexpr std::uint32_t DBUS_NAME_FLAG_DO_NOT_QUEUE = 4;
      constexpr std::uint32_t DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER = 1;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        conn_,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", SCREENCAST_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
      );
      std::uint32_t result = 0;
      if (reply) {
        g_variant_get(reply, "(u)", &result);
      }
      return result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
    }

    void emit_start_outcome(GDBusConnection *connection) const {
      switch (outcome_) {
        case start_outcome_e::stream_added:
          g_dbus_connection_emit_signal(connection, nullptr, STREAM_PATH, STREAM_IFACE, "PipeWireStreamAdded", g_variant_new("(u)", NODE_ID), nullptr);
          break;
        case start_outcome_e::stream_added_wrong_signature:
          g_dbus_connection_emit_signal(connection, nullptr, STREAM_PATH, STREAM_IFACE, "PipeWireStreamAdded", g_variant_new("(s)", "42"), nullptr);
          break;
        case start_outcome_e::session_closed:
          g_dbus_connection_emit_signal(connection, nullptr, SESSION_PATH, SESSION_IFACE, "Closed", nullptr, nullptr);
          break;
        case start_outcome_e::nothing:
          break;
      }
    }

    static void handle_method_call(GDBusConnection *connection, [[maybe_unused]] const gchar *sender, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {  // NOSONAR(cpp:S107): GDBusInterfaceVTable method calls require these parameters.
      auto *self = static_cast<fake_screencast_t *>(user_data);
      const std::string_view method {method_name};
      if (method == "Stop") {
        ++self->stop_calls;
      } else if (method == "CreateSession") {
        ++self->create_session_calls;
      }
      if (method == self->failing_method_) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.Failed", "Injected failure");
      } else if (method == "Stop") {
        g_dbus_method_invocation_return_value(invocation, nullptr);
      } else if (method == "CreateSession") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", SESSION_PATH));
      } else if (method == "RecordMonitor") {
        const gchar *requested_connector = nullptr;
        g_autoptr(GVariant) properties = nullptr;
        g_variant_get(parameters, "(&s@a{sv})", &requested_connector, &properties);
        self->connector = requested_connector;
        if (std::uint32_t requested_cursor_mode = 0; g_variant_lookup(properties, "cursor-mode", "u", &requested_cursor_mode)) {
          self->cursor_mode = requested_cursor_mode;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", STREAM_PATH));
      } else if (method == "Start") {
        g_dbus_method_invocation_return_value(invocation, nullptr);
        self->emit_start_outcome(connection);
      }
    }

    static GVariant *handle_get_property([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *property_name, GError **error, gpointer user_data) {
      const auto *self = static_cast<const fake_screencast_t *>(user_data);
      if (self->parameters_ == parameters_e::failing) {
        g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Injected failure");
        return nullptr;
      }
      GVariantBuilder stream_parameters;
      g_variant_builder_init(&stream_parameters, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&stream_parameters, "{sv}", "position", g_variant_new("(ii)", REPORTED_GEOMETRY.offset_x, REPORTED_GEOMETRY.offset_y));
      if (self->parameters_ == parameters_e::complete) {
        g_variant_builder_add(&stream_parameters, "{sv}", "size", g_variant_new("(ii)", REPORTED_GEOMETRY.width, REPORTED_GEOMETRY.height));
      } else if (self->parameters_ == parameters_e::zero_width) {
        g_variant_builder_add(&stream_parameters, "{sv}", "size", g_variant_new("(ii)", 0, REPORTED_GEOMETRY.height));
      } else if (self->parameters_ == parameters_e::zero_height) {
        g_variant_builder_add(&stream_parameters, "{sv}", "size", g_variant_new("(ii)", REPORTED_GEOMETRY.width, 0));
      }
      return g_variant_builder_end(&stream_parameters);
    }

    start_outcome_e outcome_;
    std::string failing_method_;
    parameters_e parameters_;
    bool ready_ = false;
    std::jthread thread_;
    GMainContext *context_ = nullptr;
    GMainLoop *loop_ = nullptr;
    GDBusConnection *conn_ = nullptr;
    std::vector<guint> registrations_;
  };

  /**
   * @brief Start a private message bus with GTestDBus.
   *
   * g_test_dbus_up() unsets or replaces GTEST_DBUS_ENVIRONMENT. The tests reach the bus by its
   * address only, so the previous environment is restored at once.
   *
   * GoogleTest emits test-start records before fixture setup, which calls this function. Draining
   * those records first prevents the logging formatter from reading environment-dependent state
   * concurrently with these environment changes on older glibc releases.
   *
   * @return Address of the private message bus.
   */
  std::string start_private_bus() {
    logging::log_flush();
    std::vector<std::pair<const char *, std::optional<std::string>>> saved_environment;
    for (const char *name : GTEST_DBUS_ENVIRONMENT) {
      const char *value = std::getenv(name);  // NOSONAR(cpp:S1874): `_dupenv_s` not available
      saved_environment.emplace_back(name, value ? std::optional<std::string> {value} : std::nullopt);
    }

    // Kept until the test program exits, when the GTestDBus watcher stops the bus. It is never
    // brought down: g_test_dbus_down() waits for the g_bus_get() session singleton, which other
    // tests in this program may still hold.
    static GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    std::string address = g_test_dbus_get_bus_address(bus);

    for (const auto &[name, value] : saved_environment) {
      if (value) {
        g_setenv(name, value->c_str(), TRUE);
      } else {
        g_unsetenv(name);
      }
    }
    return address;
  }

  /**
   * @brief Address of the private message bus that every test in this file shares.
   *
   * @return Address of the private message bus, started on first use.
   */
  const std::string &private_bus_address() {
    static const std::string address = start_private_bus();
    return address;
  }

  /**
   * @brief Give the niri backend test outputs and restore its sources afterwards.
   */
  class NiriTest: public BaseTest {
  protected:
    /**
     * @brief Restore the session bus, the Wayland outputs and the default timeout.
     */
    void TearDown() override {
      niri::reset_overrides_for_testing();
      BaseTest::TearDown();
    }
  };

  /**
   * @brief Run the backend against the private message bus, with DP_1 as the only output.
   */
  class NiriScreenCastTest: public NiriTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
  protected:
    /**
     * @brief Use the private message bus, or skip when dbus-daemon is not installed.
     */
    void SetUp() override {
      NiriTest::SetUp();
      if (HasFatalFailure() || IsSkipped()) {
        return;
      }
      if (g_autofree gchar *dbus_daemon = g_find_program_in_path("dbus-daemon"); !dbus_daemon) {
        GTEST_SKIP() << "dbus-daemon is not installed";
      }
      bus_address_ = private_bus_address();
      niri::set_bus_address_for_testing(bus_address_);
      niri::set_outputs_for_testing({DP_1});
    }

    /**
     * @brief Configure the stream of a niri display through the backend.
     *
     * @param stream Receives the configured stream.
     * @param display_name Connector name of the output to record, or an empty string for the first output.
     * @return Result of the backend: 0 on success, -1 on failure.
     */
    static int configure(niri::configured_stream_for_testing_t &stream, const std::string &display_name = {}) {
      return niri::configure_stream_for_testing(display_name, stream);
    }

    std::string bus_address_;  ///< Address of the private message bus.
  };

  /**
   * @brief A method that fails and whether a session existed to stop by then.
   */
  struct failing_method_t {
    const char *method;  ///< Method that answers with a D-Bus error.
    int expected_stop_calls;  ///< Stop calls expected afterwards.
  };

  /**
   * @brief Run the backend against a service where one method fails.
   */
  class NiriScreenCastFailureTest: public NiriScreenCastTest, public testing::WithParamInterface<failing_method_t> {};
}  // namespace

TEST_F(NiriScreenCastTest, StreamAddedConfiguresStreamAndStopsSession) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);
  service.shutdown();

  EXPECT_EQ(stream.pipewire_fd, -1);
  EXPECT_EQ(stream.pipewire_node, NODE_ID);
  EXPECT_EQ(stream.pipewire_object_serial, SPA_ID_INVALID);
  EXPECT_EQ(stream.offset_x, REPORTED_GEOMETRY.offset_x);
  EXPECT_EQ(stream.offset_y, REPORTED_GEOMETRY.offset_y);
  EXPECT_EQ(stream.width, REPORTED_GEOMETRY.width);
  EXPECT_EQ(stream.height, REPORTED_GEOMETRY.height);
  EXPECT_EQ(service.connector, "DP-1");
  EXPECT_EQ(service.cursor_mode, 1u);
  EXPECT_EQ(service.stop_calls, 1);
}

TEST_F(NiriScreenCastTest, SessionClosedEndsWaitWithoutTimeout) {
  fake_screencast_t service {bus_address_, start_outcome_e::session_closed};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(configure(stream), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // The backend gives up on a missing stream after 5 seconds; Closed must end the wait well before that.
  EXPECT_LT(elapsed, 2s);
}

TEST_F(NiriScreenCastTest, StreamAddedWithWrongSignatureFails) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added_wrong_signature};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(configure(stream), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // The malformed signal must end the wait, not the 5 second timeout.
  EXPECT_LT(elapsed, 2s);
}

TEST_F(NiriScreenCastTest, MissingStreamTimesOut) {
  fake_screencast_t service {bus_address_, start_outcome_e::nothing};
  ASSERT_TRUE(service.ready());
  niri::set_stream_added_timeout_for_testing(100ms);

  niri::configured_stream_for_testing_t stream;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(configure(stream), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  service.shutdown();

  EXPECT_GE(elapsed, 100ms);
  EXPECT_LT(elapsed, 2s);
  EXPECT_EQ(service.stop_calls, 1);
}

TEST_F(NiriScreenCastTest, MissingServiceFails) {
  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), -1);
}

TEST_F(NiriScreenCastTest, FailedConfigureKeepsStandardInput) {
  // Make sure fd 0 is open, so that closing it is visible even when the runner closed standard input.
  int opened_stdin = -1;
  if (struct stat unused {}; fstat(STDIN_FILENO, &unused) != 0) {
    opened_stdin = open("/dev/null", O_RDONLY);  // NOSONAR(cpp:S1874): `_sopen_s` not available
    ASSERT_EQ(opened_stdin, STDIN_FILENO);
  }
  const int saved_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
  ASSERT_GE(saved_stdin, 0);
  struct stat before {};
  ASSERT_EQ(fstat(STDIN_FILENO, &before), 0);

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), -1);

  // The display is destroyed before its PipeWire stream starts, so it owns no descriptor to close.
  struct stat after {};
  const bool stdin_open = fstat(STDIN_FILENO, &after) == 0;
  EXPECT_TRUE(stdin_open) << "the display closed fd 0";
  if (stdin_open) {
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);
  }

  // Leave fd 0 as the test found it, also after a failure.
  dup2(saved_stdin, STDIN_FILENO);
  close(saved_stdin);
  if (opened_stdin >= 0) {
    close(opened_stdin);
  }
}

TEST_F(NiriScreenCastTest, UnreachableBusFails) {
  niri::set_bus_address_for_testing("unix:path=/nonexistent/sunshine-niri-test-bus");

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), -1);
}

TEST_F(NiriScreenCastTest, FailedStopKeepsStream) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, "Stop"};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);
  service.shutdown();

  EXPECT_EQ(stream.pipewire_node, NODE_ID);
  EXPECT_EQ(service.stop_calls, 1);
}

TEST_F(NiriScreenCastTest, FailedParametersUseOutputGeometry) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, {}, parameters_e::failing};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);

  EXPECT_EQ(stream.pipewire_node, NODE_ID);
  EXPECT_EQ(stream.offset_x, DP_1.offset_x);
  EXPECT_EQ(stream.offset_y, DP_1.offset_y);
  EXPECT_EQ(stream.width, DP_1.width);
  EXPECT_EQ(stream.height, DP_1.height);
}

TEST_F(NiriScreenCastTest, IncompleteParametersUseOutputGeometry) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, {}, parameters_e::incomplete};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);

  EXPECT_EQ(stream.offset_x, DP_1.offset_x);
  EXPECT_EQ(stream.offset_y, DP_1.offset_y);
  EXPECT_EQ(stream.width, DP_1.width);
  EXPECT_EQ(stream.height, DP_1.height);
}

TEST_F(NiriScreenCastTest, ZeroWidthParametersUseOutputGeometry) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, {}, parameters_e::zero_width};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);

  EXPECT_EQ(stream.offset_x, DP_1.offset_x);
  EXPECT_EQ(stream.offset_y, DP_1.offset_y);
  EXPECT_EQ(stream.width, DP_1.width);
  EXPECT_EQ(stream.height, DP_1.height);
}

TEST_F(NiriScreenCastTest, ZeroHeightParametersUseOutputGeometry) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, {}, parameters_e::zero_height};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), 0);

  EXPECT_EQ(stream.offset_x, DP_1.offset_x);
  EXPECT_EQ(stream.offset_y, DP_1.offset_y);
  EXPECT_EQ(stream.width, DP_1.width);
  EXPECT_EQ(stream.height, DP_1.height);
}

TEST_F(NiriScreenCastTest, NamedOutputIsRecorded) {
  const niri::output_for_testing_t hdmi {"HDMI-A-1", 2020, 50, 2560, 1440};
  niri::set_outputs_for_testing({DP_1, hdmi});
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, {}, parameters_e::failing};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream, "HDMI-A-1"), 0);
  service.shutdown();

  EXPECT_EQ(service.connector, "HDMI-A-1");
  EXPECT_EQ(stream.offset_x, hdmi.offset_x);
  EXPECT_EQ(stream.width, hdmi.width);
}

TEST_F(NiriScreenCastTest, UnknownOutputFallsBackToLeftMostOutput) {
  niri::set_outputs_for_testing({{"HDMI-A-1", 2020, 50, 2560, 1440}, DP_1});
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream, "DP-9"), 0);
  service.shutdown();

  EXPECT_EQ(service.connector, "DP-1");
}

TEST_F(NiriScreenCastTest, NoNamedOutputFailsWithoutSession) {
  niri::set_outputs_for_testing({{"", 0, 0, 1920, 1080}});
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  EXPECT_EQ(configure(stream), -1);
  service.shutdown();

  EXPECT_EQ(service.create_session_calls, 0);
}

TEST_F(NiriScreenCastTest, AvailableWhenServiceIsOnBus) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added};
  ASSERT_TRUE(service.ready());

  EXPECT_TRUE(platf::niri_available());
}

TEST_F(NiriScreenCastTest, UnavailableWithoutService) {
  EXPECT_FALSE(platf::niri_available());
}

TEST_F(NiriScreenCastTest, UnavailableWithoutBus) {
  niri::set_bus_address_for_testing("unix:path=/nonexistent/sunshine-niri-test-bus");

  EXPECT_FALSE(platf::niri_available());
}

TEST_P(NiriScreenCastFailureTest, FailedCallFailsPromptlyAndStopsCreatedSession) {
  fake_screencast_t service {bus_address_, start_outcome_e::stream_added, GetParam().method};
  ASSERT_TRUE(service.ready());

  niri::configured_stream_for_testing_t stream;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(configure(stream), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  service.shutdown();

  EXPECT_LT(elapsed, 2s);
  EXPECT_EQ(service.stop_calls, GetParam().expected_stop_calls);
}

INSTANTIATE_TEST_SUITE_P(
  FailingMethods,
  NiriScreenCastFailureTest,
  testing::Values(failing_method_t {"CreateSession", 0}, failing_method_t {"RecordMonitor", 1}, failing_method_t {"Start", 1}),
  [](const testing::TestParamInfo<failing_method_t> &info) {
    return std::string {info.param.method};
  }
);

TEST_F(NiriTest, DisplayNamesSkipUnnamedOutputsAndSortByPosition) {
  niri::set_outputs_for_testing({{"HDMI-A-1", 1920, 0, 2560, 1440}, {"", 0, 0, 800, 600}, {"DP-2", 0, 1080, 1920, 1080}, {"DP-1", 0, 0, 1920, 1080}});

  EXPECT_EQ(platf::niri_display_names(), (std::vector<std::string> {"DP-1", "DP-2", "HDMI-A-1"}));
}
#endif
