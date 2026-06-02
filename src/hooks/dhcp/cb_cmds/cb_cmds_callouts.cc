// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <cb_cmds.h>
#include <cb_cmds_log.h>
#include <dhcpsrv/cfgmgr.h>
#include <hooks/hooks.h>
#include <process/daemon.h>

#include <string>

using namespace isc::cb_cmds;
using namespace isc::dhcp;
using namespace isc::hooks;
using namespace isc::process;
using namespace std;

extern "C" {

int
cb_cmds_command(CalloutHandle& handle) {
    try {
        CbCmds commands;
        commands.handleCommand(handle);
    } catch (const exception& ex) {
        LOG_ERROR(cb_cmds_logger, CB_CMDS_HANDLER_FAILED).arg(ex.what());
        return (1);
    }
    return (0);
}

int
load(LibraryHandle& handle) {
    try {
        uint16_t family = CfgMgr::instance().getFamily();
        const string& proc_name = Daemon::getProcName();
        if ((family == AF_INET) && (proc_name != "kea-dhcp4")) {
            isc_throw(isc::Unexpected, "Bad process name: " << proc_name
                      << ", expected kea-dhcp4");
        } else if ((family == AF_INET6) && (proc_name != "kea-dhcp6")) {
            isc_throw(isc::Unexpected, "Bad process name: " << proc_name
                      << ", expected kea-dhcp6");
        }

        for (auto const& command : commandNames(family)) {
            handle.registerCommandCallout(command, cb_cmds_command);
        }
    } catch (const exception& ex) {
        LOG_ERROR(cb_cmds_logger, CB_CMDS_INIT_FAILED).arg(ex.what());
        return (1);
    }

    LOG_INFO(cb_cmds_logger, CB_CMDS_INIT_OK);
    return (0);
}

int
unload() {
    LOG_INFO(cb_cmds_logger, CB_CMDS_DEINIT_OK);
    return (0);
}

int
multi_threading_compatible() {
    return (1);
}

} // end extern "C"
