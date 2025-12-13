#include <xz/redis/detail/connection_fsm.hpp>
#include <gtest/gtest.h>

using namespace xz::redis;

// Helper to convert config to handshake_plan for testing
static auto make_plan(config const& cfg, bool needs_hello = true) -> handshake_plan {
  return handshake_plan{
      .needs_hello = needs_hello,
      .needs_auth = cfg.password.has_value(),
      .needs_select_db = cfg.database != 0,
      .needs_set_clientname = cfg.client_name.has_value(),
  };
}

class ConnectionFsmTest : public ::testing::Test {
 protected:
  auto get_action_type(fsm_action_variant const& action) -> std::string {
    return std::visit(
        [](auto const& a) -> std::string {
          using T = std::decay_t<decltype(a)>;
          if constexpr (std::is_same_v<T, fsm_action::state_change>) {
            return "state_change";
          } else if constexpr (std::is_same_v<T, fsm_action::send_hello>) {
            return "send_hello";
          } else if constexpr (std::is_same_v<T, fsm_action::send_auth>) {
            return "send_auth";
          } else if constexpr (std::is_same_v<T, fsm_action::send_select>) {
            return "send_select";
          } else if constexpr (std::is_same_v<T, fsm_action::send_clientname>) {
            return "send_clientname";
          } else if constexpr (std::is_same_v<T, fsm_action::connection_ready>) {
            return "connection_ready";
          } else if constexpr (std::is_same_v<T, fsm_action::connection_failed>) {
            return "connection_failed";
          }
          return "unknown";
        },
        action);
  }

  auto has_action(fsm_output const& actions, std::string const& type) -> bool {
    for (auto const& action : actions) {
      if (get_action_type(action) == type) {
        return true;
      }
    }
    return false;
  }
};

TEST_F(ConnectionFsmTest, InitialStateIsDisconnected) {
  handshake_plan plan{.needs_hello = true, .needs_auth = false, .needs_select_db = false, .needs_set_clientname = false};
  connection_fsm fsm{plan};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);
}

TEST_F(ConnectionFsmTest, OnConnectedSendsHelloAndTransitionsToHandshaking) {
  handshake_plan plan{.needs_hello = true, .needs_auth = false, .needs_select_db = false, .needs_set_clientname = false};
  connection_fsm fsm{plan};

  auto output = fsm.on_connected();

  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
  EXPECT_TRUE(has_action(output, "send_hello"));
}

TEST_F(ConnectionFsmTest, HelloOkWithoutAuthGoesToReady) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto output = fsm.on_hello_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(output, "connection_ready"));
}

TEST_F(ConnectionFsmTest, HelloOkWithAuthGoesToAuthenticating) {
  config cfg{.password = "secret"};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto output = fsm.on_hello_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);
  EXPECT_TRUE(has_action(output, "send_auth"));
}

TEST_F(ConnectionFsmTest, HelloOkWithUsernamePasswordAuth) {
  config cfg{
      .username = "admin",
      .password = "secret",
  };
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto output = fsm.on_hello_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);
  EXPECT_TRUE(has_action(output, "send_auth"));
}

TEST_F(ConnectionFsmTest, AuthOkGoesToReady) {
  config cfg{.password = "secret"};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  fsm.on_hello_ok();
  auto output = fsm.on_auth_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(output, "connection_ready"));
}

TEST_F(ConnectionFsmTest, HelloErrorGoesToFailed) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto output = fsm.on_hello_error(make_error_code(error::resp3_hello));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));

  // Check error code is preserved
  for (auto const& action : output) {
    if (auto* failed = std::get_if<fsm_action::connection_failed>(&action)) {
      EXPECT_EQ(failed->ec, make_error_code(error::resp3_hello));
    }
  }
}

TEST_F(ConnectionFsmTest, AuthErrorGoesToFailed) {
  config cfg{.password = "wrong-password"};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  fsm.on_hello_ok();
  auto output = fsm.on_auth_error(make_error_code(error::auth_failed));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));
}

TEST_F(ConnectionFsmTest, IoErrorGoesToFailed) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  auto output = fsm.on_io_error(make_error_code(error::connect_timeout));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));

  // Check error code is preserved
  for (auto const& action : output) {
    if (auto* failed = std::get_if<fsm_action::connection_failed>(&action)) {
      EXPECT_EQ(failed->ec, make_error_code(error::connect_timeout));
    }
  }
}

TEST_F(ConnectionFsmTest, ResetBringsBackToDisconnected) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  fsm.reset();
  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);
}

TEST_F(ConnectionFsmTest, FullSuccessfulFlowWithoutAuth) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);

  auto out1 = fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
  EXPECT_TRUE(has_action(out1, "send_hello"));

  auto out2 = fsm.on_hello_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(out2, "connection_ready"));
}

TEST_F(ConnectionFsmTest, FullSuccessfulFlowWithAuth) {
  config cfg{.password = "secret"};
  connection_fsm fsm{make_plan(cfg)};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto out2 = fsm.on_hello_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);
  EXPECT_TRUE(has_action(out2, "send_auth"));

  auto out3 = fsm.on_auth_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(out3, "connection_ready"));
}

TEST_F(ConnectionFsmTest, OnConnectedFromNonDisconnectedStateDoesNothing) {
  config cfg;
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto output = fsm.on_connected();
  EXPECT_EQ(output.size(), 0);  // No state change, no actions
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
}

TEST_F(ConnectionFsmTest, SelectDatabaseFlow) {
  config cfg{.database = 2};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto out1 = fsm.on_hello_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::selecting_db);
  EXPECT_TRUE(has_action(out1, "send_select"));

  auto out2 = fsm.on_select_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(out2, "connection_ready"));
}

TEST_F(ConnectionFsmTest, SelectErrorGoesToFailed) {
  config cfg{.database = 999};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  fsm.on_hello_ok();
  auto output = fsm.on_select_error(make_error_code(error::select_db_failed));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));
}

TEST_F(ConnectionFsmTest, SetClientNameFlow) {
  config cfg{.client_name = "my-app"};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  auto out1 = fsm.on_hello_ok();

  EXPECT_EQ(fsm.current_state(), connection_state::setting_clientname);
  EXPECT_TRUE(has_action(out1, "send_clientname"));

  auto out2 = fsm.on_clientname_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(out2, "connection_ready"));
}

TEST_F(ConnectionFsmTest, ClientnameErrorGoesToFailed) {
  config cfg{.client_name = "test"};
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  fsm.on_hello_ok();
  auto output = fsm.on_clientname_error(make_error_code(error::client_setname_failed));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));
}

TEST_F(ConnectionFsmTest, CompleteFlowWithAllOptions) {
  config cfg{
      .password = "secret",
      .database = 1,
      .client_name = "test-client",
  };
  connection_fsm fsm{make_plan(cfg)};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  fsm.on_hello_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  fsm.on_auth_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::selecting_db);

  fsm.on_select_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::setting_clientname);

  auto final_out = fsm.on_clientname_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(final_out, "connection_ready"));
}

// Test removed: EventsInWrongStateAreIgnored
// In debug builds, wrong-state events trigger assertions to catch logic errors.
// In release builds, they are silently ignored for defensive robustness.
// This is by design - assertions help catch bugs during development.

TEST_F(ConnectionFsmTest, Resp2ModeSkipsHandshakingState) {
  config cfg;
  connection_fsm fsm{make_plan(cfg, false)};  // needs_hello = false for RESP2

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);

  auto output = fsm.on_connected();

  // Should skip handshaking and go directly to ready (no auth, no db, no clientname)
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_TRUE(has_action(output, "state_change"));
  EXPECT_TRUE(has_action(output, "connection_ready"));
  EXPECT_FALSE(has_action(output, "send_hello"));  // No HELLO in RESP2
}

TEST_F(ConnectionFsmTest, Resp2WithAuthGoesToAuthenticating) {
  config cfg{.password = "secret"};
  connection_fsm fsm{make_plan(cfg, false)};  // needs_hello = false for RESP2

  auto output = fsm.on_connected();

  // Should skip handshaking and go to authenticating
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);
  EXPECT_TRUE(has_action(output, "state_change"));
  EXPECT_TRUE(has_action(output, "send_auth"));
  EXPECT_FALSE(has_action(output, "send_hello"));  // No HELLO in RESP2
}

TEST_F(ConnectionFsmTest, IoErrorCanHappenInAnyState) {
  config cfg{.password = "secret"};
  connection_fsm fsm{make_plan(cfg)};

  // Error during handshaking
  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto output = fsm.on_io_error(make_error_code(error::pong_timeout));
  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output, "connection_failed"));

  // Reset and try error during auth
  fsm.reset();
  fsm.on_connected();
  fsm.on_hello_ok();
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  auto output2 = fsm.on_io_error(make_error_code(error::pong_timeout));
  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  EXPECT_TRUE(has_action(output2, "connection_failed"));
}

TEST_F(ConnectionFsmTest, ActionsAreDataFree) {
  config cfg{
      .username = "admin",
      .password = "secret",
      .database = 5,
      .client_name = "my-app",
  };
  connection_fsm fsm{make_plan(cfg)};

  // All actions should be data-free structs, state_change should have old/new states
  auto out1 = fsm.on_connected();
  ASSERT_EQ(out1.size(), 2);  // state_change + send_hello
  ASSERT_TRUE(std::holds_alternative<fsm_action::state_change>(out1[0]));
  auto sc1 = std::get<fsm_action::state_change>(out1[0]);
  EXPECT_EQ(sc1.old_state, connection_state::disconnected);
  EXPECT_EQ(sc1.new_state, connection_state::handshaking);
  EXPECT_TRUE(std::holds_alternative<fsm_action::send_hello>(out1[1]));

  auto out2 = fsm.on_hello_ok();
  ASSERT_EQ(out2.size(), 2);  // state_change + send_auth
  ASSERT_TRUE(std::holds_alternative<fsm_action::state_change>(out2[0]));
  auto sc2 = std::get<fsm_action::state_change>(out2[0]);
  EXPECT_EQ(sc2.old_state, connection_state::handshaking);
  EXPECT_EQ(sc2.new_state, connection_state::authenticating);
  EXPECT_TRUE(std::holds_alternative<fsm_action::send_auth>(out2[1]));

  auto out3 = fsm.on_auth_ok();
  ASSERT_EQ(out3.size(), 2);  // state_change + send_select
  ASSERT_TRUE(std::holds_alternative<fsm_action::state_change>(out3[0]));
  auto sc3 = std::get<fsm_action::state_change>(out3[0]);
  EXPECT_EQ(sc3.old_state, connection_state::authenticating);
  EXPECT_EQ(sc3.new_state, connection_state::selecting_db);
  EXPECT_TRUE(std::holds_alternative<fsm_action::send_select>(out3[1]));

  auto out4 = fsm.on_select_ok();
  ASSERT_EQ(out4.size(), 2);  // state_change + send_clientname
  ASSERT_TRUE(std::holds_alternative<fsm_action::state_change>(out4[0]));
  auto sc4 = std::get<fsm_action::state_change>(out4[0]);
  EXPECT_EQ(sc4.old_state, connection_state::selecting_db);
  EXPECT_EQ(sc4.new_state, connection_state::setting_clientname);
  EXPECT_TRUE(std::holds_alternative<fsm_action::send_clientname>(out4[1]));

  auto out5 = fsm.on_clientname_ok();
  ASSERT_EQ(out5.size(), 2);  // state_change + connection_ready
  ASSERT_TRUE(std::holds_alternative<fsm_action::state_change>(out5[0]));
  auto sc5 = std::get<fsm_action::state_change>(out5[0]);
  EXPECT_EQ(sc5.old_state, connection_state::setting_clientname);
  EXPECT_EQ(sc5.new_state, connection_state::ready);
  EXPECT_TRUE(std::holds_alternative<fsm_action::connection_ready>(out5[1]));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
