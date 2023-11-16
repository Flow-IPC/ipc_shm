/* Flow-IPC: Shared Memory
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

#include "common.hpp"
#include "schema.capnp.h"
#include <ipc/transport/bipc_mq_handle.hpp>
#include <ipc/session/shm/classic/client_session.hpp>
#include <flow/log/simple_ostream_logger.hpp>
#include <flow/log/async_file_logger.hpp>

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main(int, char const * const * argv)
{
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::Error_code;
  using flow::Flow_log_component;

  using boost::promise;

  using std::string;
  using std::exception;

  const string LOG_FILE = "ipc_shm_link_test_cli.log";
  const int BAD_EXIT = 1;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  Config std_log_config;
  std_log_config.init_component_to_union_idx_mapping<Flow_log_component>(1000, 999);
  std_log_config.init_component_names<Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "link_test-");

  Simple_ostream_logger std_logger(&std_log_config);
  FLOW_LOG_SET_CONTEXT(&std_logger, Flow_log_component::S_UNCAT);

  // This is separate: the IPC/Flow logging will go into this file.
  FLOW_LOG_INFO("Opening log file [" << LOG_FILE << "] for IPC/Flow logs only.");
  Config log_config = std_log_config;
  log_config.configure_default_verbosity(Sev::S_INFO, true);
  /* First arg: could use &std_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(nullptr, &log_config, LOG_FILE, false /* No rotation; we're no serious business. */);

  try
  {
    ensure_run_env(argv[0], false);

    // Please see main_cli.cpp.  We're just the other side of that.  Keeping comments light.

    /* @todo This uses promises/futures to avoid having to make a thread/event loop; this avoidance is allowed though
     * informally discouraged by Flow-IPC docs; and really making a Single_threaded_event_loop is easy and
     * would probably make for nicer code.  It's only a sanity test, so whatever, but still....
     * E.g., ipc_transport_structured link_test uses a thread loop. */

    using Session = ipc::session::shm::classic::Client_session<ipc::session::schema::MqType::BIPC, false>;
    Session session(&log_logger,
                    CLI_APPS.find(CLI_NAME)->second,
                    SRV_APPS.find(SRV_NAME)->second, [](const Error_code&) {});

    FLOW_LOG_INFO("Session-client attempting to open session against session-server; "
                  "it'll either succeed or fail very soon; on success at that point we will receive a message and "
                  "exit.");

    bool ok = false;
    promise<void> connected_promise;
    Session::Channels chans;
    session.async_connect(session.mdt_builder(), nullptr, nullptr, &chans,
                          [&](const Error_code& err_code)
    {
      if (err_code)
      {
        FLOW_LOG_WARNING("Connect failed (perhaps you did not execute session-server executable in parallel, or "
                         "you executed one or both of us oddly?).  "
                         "Error: [" << err_code << "] [" << err_code.message() << "].");
      }
      else
      {
        FLOW_LOG_INFO("Session/channels opened.  Awaiting one message; then exiting.");
        ok = true;
      }
      connected_promise.set_value();
    });

    connected_promise.get_future().wait();
    if (!ok)
    {
      return BAD_EXIT;
    }
    // else

    promise<void> done_promise;

    Session::Structured_channel<link_test::FunBody>
      chan(&log_logger, std::move(chans.front()),
           ipc::transport::struc::Channel_base::S_SERIALIZE_VIA_SESSION_SHM, &session);
    chan.start([](const Error_code&) {});

    chan.expect_msg(link_test::FunBody::COOL_MSG, [&](auto&& msg)
    {
      FLOW_LOG_INFO("Message received with payload [" << msg->body_root().getCoolMsg().getCoolVal() << "].");
      done_promise.set_value();
    });

    done_promise.get_future().wait();

    FLOW_LOG_INFO("Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
} // main()
