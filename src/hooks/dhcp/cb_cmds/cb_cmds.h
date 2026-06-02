// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef CB_CMDS_H
#define CB_CMDS_H

#include <hooks/hooks.h>

#include <stdint.h>
#include <string>
#include <vector>

namespace isc {
namespace cb_cmds {

/// @brief Returns all Configuration Backend command names implemented by this hook.
std::vector<std::string> commandNames(const uint16_t family = 0);

/// @brief Implements Configuration Backend control commands.
class CbCmds {
public:
    /// @brief Processes a cb_cmds control command.
    ///
    /// @param handle callout handle holding the command and response.
    void handleCommand(hooks::CalloutHandle& handle) const;
};

} // end of namespace isc::cb_cmds
} // end of namespace isc

#endif // CB_CMDS_H
