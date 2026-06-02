// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>
#include <cb_cmds_messages.h>
#include <log/message_initializer.h>

const isc::log::MessageID CB_CMDS_DEINIT_OK = "CB_CMDS_DEINIT_OK";
const isc::log::MessageID CB_CMDS_HANDLER_FAILED = "CB_CMDS_HANDLER_FAILED";
const isc::log::MessageID CB_CMDS_INIT_FAILED = "CB_CMDS_INIT_FAILED";
const isc::log::MessageID CB_CMDS_INIT_OK = "CB_CMDS_INIT_OK";

namespace {

const char* values[] = {
    "CB_CMDS_DEINIT_OK", "unloading Configuration Backend Commands hooks library",
    "CB_CMDS_HANDLER_FAILED", "Configuration Backend command handler failed: %1",
    "CB_CMDS_INIT_FAILED", "loading Configuration Backend Commands hooks library failed: %1",
    "CB_CMDS_INIT_OK", "loaded Configuration Backend Commands hooks library",
    nullptr
};

const isc::log::MessageInitializer initializer(values);

} // end of anonymous namespace
