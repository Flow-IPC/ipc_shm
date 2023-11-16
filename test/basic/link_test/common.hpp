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

#include <ipc/session/app.hpp>
#include <boost/filesystem/path.hpp>
#include <string>

namespace fs = boost::filesystem;

extern const fs::path WORK_DIR;

// Common ipc::session::App-related data used on both sides (the "IPC universe" description).
extern const std::string SRV_NAME;
extern const std::string CLI_NAME;
extern const ipc::session::Server_app::Master_set SRV_APPS;
extern const ipc::session::Client_app::Master_set CLI_APPS;

// Invoke from main() from either application to ensure it's being run directly from the expected CWD.
void ensure_run_env(const char* argv0, bool srv_else_cli);
