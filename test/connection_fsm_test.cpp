#include <xz/redis/connection_fsm.hpp>
#include <gtest/gtest.h>

using namespace xz::redis;

class ConnectionFsmTest : public ::testing::Test {
 protected:
  auto get_action_type(fsm_action_variant const& action) -> std::string {
    return std::visit(
        [](auto const& a) -> std::string {
          using T = std::decay_t<decltype(a)>;
          if constexpr (std::is_same_v<T, fsm_action::send_data>) {
            return "send_data";
          } else if constexpr (std::is_same_v<T, fsm_action::state_changed>) {
            return "state_changed";
          } else if constexpr (std::is_same_v<T, fsm_action::connection_ready>) {
            return "connection_ready";
          } else if constexpr (std::is_same_v<T, fsm_action::connection_failed>) {
            return "connection_failed";
          }
          return "unknown";
        },
        action);
  }

  auto make_ok_response() -> std::string { return "+OK\r\n"; }

  auto make_map_response() -> std::string {
    return "%7\r\n"
           "+server\r\n+redis\r\n"
           "+version\r\n+7.0.0\r\n"
           "+proto\r\n:3\r\n"
           "+id\r\n:1\r\n"
           "+mode\r\n+standalone\r\n"
           "+role\r\n+master\r\n"
           "+modules\r\n*0\r\n";
  }

  auto make_error_response() -> std::string { return "-ERR unknown command\r\n"; }

  auto process_data(connection_fsm& fsm, std::string const& data) -> fsm_output {
    resp3::parser parser;
    parser.feed(data);
    auto gen = parser.parse();

    fsm_output combined;
    while (gen.next()) {
      auto msg_opt = gen.value();
      if (!msg_opt) {
        break;
      }
      if (!msg_opt->empty()) {
        auto out = fsm.on_data_received(*msg_opt);
        for (auto& action : out.actions) {
          combined.actions.emplace_back(std::move(action));
        }
      }
    }
    return combined;
  }
};

TEST_F(ConnectionFsmTest, InitialStateIsDisconnected) {
  config cfg;
  connection_fsm fsm{cfg};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);
}

TEST_F(ConnectionFsmTest, OnConnectedSendsHelloCommand) {
  config cfg;
  connection_fsm fsm{cfg};

  auto output = fsm.on_connected();

  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
  ASSERT_GE(output.actions.size(), 1);

  bool found_send = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "send_data") {
      auto const& send = std::get<fsm_action::send_data>(action);
      EXPECT_TRUE(send.data.find("HELLO") != std::string::npos);
      EXPECT_TRUE(send.data.find("3") != std::string::npos);
      found_send = true;
    }
  }
  EXPECT_TRUE(found_send);
}

TEST_F(ConnectionFsmTest, HelloResponseWithoutAuthGoesToReady) {
  config cfg;
  connection_fsm fsm{cfg};

  fsm.on_connected();

  auto hello_response = make_map_response();
  auto output = process_data(fsm, hello_response);

  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  bool found_ready = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "connection_ready") {
      found_ready = true;
    }
  }
  EXPECT_TRUE(found_ready);
}

TEST_F(ConnectionFsmTest, HelloResponseWithAuthGoesToAuthenticating) {
  config cfg{
      .password = "secret",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();

  auto hello_response = make_map_response();
  auto output = process_data(fsm, hello_response);

  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  bool found_send = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "send_data") {
      auto const& send = std::get<fsm_action::send_data>(action);
      EXPECT_TRUE(send.data.find("AUTH") != std::string::npos);
      EXPECT_TRUE(send.data.find("secret") != std::string::npos);
      found_send = true;
    }
  }
  EXPECT_TRUE(found_send);
}

TEST_F(ConnectionFsmTest, HelloResponseWithUsernamePasswordAuth) {
  config cfg{
      .username = "admin",
      .password = "secret",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();

  auto hello_response = make_map_response();
  auto output = process_data(fsm, hello_response);

  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  bool found_send = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "send_data") {
      auto const& send = std::get<fsm_action::send_data>(action);
      EXPECT_TRUE(send.data.find("AUTH") != std::string::npos);
      EXPECT_TRUE(send.data.find("admin") != std::string::npos);
      EXPECT_TRUE(send.data.find("secret") != std::string::npos);
      found_send = true;
    }
  }
  EXPECT_TRUE(found_send);
}

TEST_F(ConnectionFsmTest, AuthResponseGoesToReady) {
  config cfg{
      .password = "secret",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();
  auto hello_response = make_map_response();
  process_data(fsm, hello_response);

  auto auth_response = make_ok_response();
  auto output = process_data(fsm, auth_response);

  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  bool found_ready = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "connection_ready") {
      found_ready = true;
    }
  }
  EXPECT_TRUE(found_ready);
}

TEST_F(ConnectionFsmTest, HelloErrorResponseGoesToFailed) {
  config cfg;
  connection_fsm fsm{cfg};

  fsm.on_connected();

  auto error_response = make_error_response();
  auto output = process_data(fsm, error_response);

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  bool found_failed = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "connection_failed") {
      found_failed = true;
    }
  }
  EXPECT_TRUE(found_failed);
}

TEST_F(ConnectionFsmTest, AuthErrorResponseGoesToFailed) {
  config cfg{
      .password = "wrong-password",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();
  auto hello_response = make_map_response();
  process_data(fsm, hello_response);

  auto error_response = make_error_response();
  auto output = process_data(fsm, error_response);

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  bool found_failed = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "connection_failed") {
      found_failed = true;
    }
  }
  EXPECT_TRUE(found_failed);
}

TEST_F(ConnectionFsmTest, OnConnectionFailedGoesToFailed) {
  config cfg;
  connection_fsm fsm{cfg};

  auto output = fsm.on_connection_failed(make_error_code(error::connect_timeout));

  EXPECT_EQ(fsm.current_state(), connection_state::failed);
  bool found_failed = false;
  for (auto const& action : output.actions) {
    if (get_action_type(action) == "connection_failed") {
      auto const& failed = std::get<fsm_action::connection_failed>(action);
      EXPECT_EQ(failed.ec, make_error_code(error::connect_timeout));
      found_failed = true;
    }
  }
  EXPECT_TRUE(found_failed);
}

TEST_F(ConnectionFsmTest, ResetBringsBackToDisconnected) {
  config cfg;
  connection_fsm fsm{cfg};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  fsm.reset();
  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);
}

TEST_F(ConnectionFsmTest, FullSuccessfulFlowWithoutAuth) {
  config cfg;
  connection_fsm fsm{cfg};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);

  auto out1 = fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
  EXPECT_GE(out1.actions.size(), 1);

  auto hello_response = make_map_response();
  auto out2 = process_data(fsm, hello_response);
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_GE(out2.actions.size(), 1);
}

TEST_F(ConnectionFsmTest, FullSuccessfulFlowWithAuth) {
  config cfg{
      .password = "secret",
  };
  connection_fsm fsm{cfg};

  EXPECT_EQ(fsm.current_state(), connection_state::disconnected);

  auto out1 = fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto hello_response = make_map_response();
  auto out2 = process_data(fsm, hello_response);
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  auto auth_response = make_ok_response();
  auto out3 = process_data(fsm, auth_response);
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
  EXPECT_GE(out3.actions.size(), 1);
}

TEST_F(ConnectionFsmTest, OnConnectedFromNonDisconnectedStateDoesNothing) {
  config cfg;
  connection_fsm fsm{cfg};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto output = fsm.on_connected();
  EXPECT_EQ(output.actions.size(), 0);
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);
}

TEST_F(ConnectionFsmTest, SelectDatabaseFlow) {
  config cfg{
      .database = 2,
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();
  auto hello_response = make_map_response();
  auto out1 = process_data(fsm, hello_response);

  EXPECT_EQ(fsm.current_state(), connection_state::selecting_db);

  bool found_select = false;
  for (auto const& action : out1.actions) {
    if (get_action_type(action) == "send_data") {
      auto const& send = std::get<fsm_action::send_data>(action);
      if (send.data.find("SELECT") != std::string::npos) {
        found_select = true;
      }
    }
  }
  EXPECT_TRUE(found_select);

  auto select_response = make_ok_response();
  auto out2 = process_data(fsm, select_response);
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
}

TEST_F(ConnectionFsmTest, SetClientNameFlow) {
  config cfg{
      .client_name = "my-app",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();
  auto hello_response = make_map_response();
  auto out1 = process_data(fsm, hello_response);

  EXPECT_EQ(fsm.current_state(), connection_state::setting_clientname);

  bool found_setname = false;
  for (auto const& action : out1.actions) {
    if (get_action_type(action) == "send_data") {
      auto const& send = std::get<fsm_action::send_data>(action);
      if (send.data.find("CLIENT") != std::string::npos && send.data.find("SETNAME") != std::string::npos) {
        found_setname = true;
      }
    }
  }
  EXPECT_TRUE(found_setname);

  auto setname_response = make_ok_response();
  auto out2 = process_data(fsm, setname_response);
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
}

TEST_F(ConnectionFsmTest, CompleteFlowWithAllOptions) {
  config cfg{
      .password = "secret",
      .database = 1,
      .client_name = "test-client",
  };
  connection_fsm fsm{cfg};

  fsm.on_connected();
  EXPECT_EQ(fsm.current_state(), connection_state::handshaking);

  auto hello_response = make_map_response();
  process_data(fsm, hello_response);
  EXPECT_EQ(fsm.current_state(), connection_state::authenticating);

  auto auth_response = make_ok_response();
  process_data(fsm, auth_response);
  EXPECT_EQ(fsm.current_state(), connection_state::selecting_db);

  auto select_response = make_ok_response();
  process_data(fsm, select_response);
  EXPECT_EQ(fsm.current_state(), connection_state::setting_clientname);

  auto setname_response = make_ok_response();
  process_data(fsm, setname_response);
  EXPECT_EQ(fsm.current_state(), connection_state::ready);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
