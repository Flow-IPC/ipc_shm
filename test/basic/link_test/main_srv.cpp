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
#include <ipc/session/shm/classic/session_server.hpp>
#include <flow/log/simple_ostream_logger.hpp>
#include <flow/log/async_file_logger.hpp>

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main(int argc, char const * const * argv)
{
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::Error_code;
  using flow::Flow_log_component;
  using flow::error::Runtime_error;
  using boost::promise;
  using std::exception;
  using std::optional;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  optional<Simple_ostream_logger> std_logger;
  optional<Async_file_logger> log_logger;
  setup_log_cfg(&std_logger, &log_logger, argc, argv, false);
  FLOW_LOG_SET_CONTEXT(&(*std_logger), Flow_log_component::S_UNCAT);

  try
  {
    /* This test is similar to ipc_session's (it's our immediate dependency) with 2 key differences:
     *   - The sessions are SHM-enabled.  This just means we add `shm::classic::` into Session_server type,
     *     while main_cli does same for Client_session.  Yay!
     *   - We actively use the SHM-enabledness by actually using the session by opening a channel and sending
     *     a zero-copy message over it.
     *     - In this aspect it's a bit like ipc_transport_structured's link_test; but that one set up the channel
     *       manually/painfully without the ability to use sessions -- or SHM-backing (zero-copy end-to-end).
     * It's a way to sanity-check SHM capabilities, which involve various shm_*() calls, so it's a functional
     * and build/link test.
     *
     * Keeping comments light other than the new stuff. */

    ensure_run_env(argv[0], true);

    using Session_server = ipc::session::shm::classic::Session_server<ipc::session::schema::MqType::BIPC, false>;
    Session_server srv(&(*log_logger), SRV_APPS.find(SRV_NAME)->second, CLI_APPS);

    FLOW_LOG_INFO("Session-server started; invoke session-client executable from same CWD; it will open session with "
                  "1 init-channel; at that point we will send a message and be satisfied and exit.");

    /* @todo This uses promises/futures to avoid having to make a thread/event loop; this avoidance is allowed though
     * informally discouraged by Flow-IPC docs; and really making a Single_threaded_event_loop is easy and
     * would probably make for nicer code.  It's only a sanity test, so whatever, but still....
     * E.g., ipc_transport_structured link_test uses a thread loop. */

    using Session = decltype(srv)::Server_session_obj;
    Session session;
    promise<Error_code> accepted_promise;
    Session_server::Channels chans;
    srv.async_accept(&session, &chans, nullptr, nullptr,
                     [](auto&&...) -> size_t { return 1; }, // 1 init-channel to open.
                     [](auto&&...) {},
                     [&](const Error_code& async_err_code)
    {
      accepted_promise.set_value(async_err_code);
    });

    if (accepted_promise.get_future().get())
    {
      throw Runtime_error(err_code, "totally unexpected error while accepting");
    }
    // else
    FLOW_LOG_INFO("Session accepted: [" << session << "].");

    session.init_handlers([](const Error_code&) {});
    /* Session in PEER state (opened fully); so channel is ready too.  Upgrade to struc::Channel; then send a
     * (SHM-backed) message. */

    /* BTW compare to the simplicity of this type+ctor signature/call versus ipc_transport_structured's main.cpp.
     * That's ipc::session's presence at work. */
    Session::Structured_channel<link_test::FunBody>
      chan(&(*log_logger), std::move(chans.front()),
           ipc::transport::struc::Channel_base::S_SERIALIZE_VIA_SESSION_SHM, &session);
    chan.start([](auto&&...) {});

    auto msg = chan.create_msg();
    auto msg_root = msg.body_root()->initCoolMsg();
    msg_root.setCoolVal(42);
    msg_root.setCoolString("Hello, world!");
    FLOW_LOG_INFO("Sending a structured message over pre-opened channel.");
    chan.send(msg);

    // Don't judge us.  Again, we aren't demo-ing best practices here!
    FLOW_LOG_INFO("Sleeping for a few sec to avoid yanking channel away from other side right after opening it.  "
                  "This is not intended to demonstrate a best practice -- just acting a certain way in a "
                  "somewhat contrived short-lived-session scenario; essentially so that on the client side it "
                  "can \"savor\" the newly-open session/channel, before we take them down right away.");
    flow::util::this_thread::sleep_for(boost::chrono::seconds(1));

    FLOW_LOG_INFO("Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return 1;
  }

  return 0;
} // main()
