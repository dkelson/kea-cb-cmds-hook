// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <cc/data.h>
#include <config/command_mgr.h>
#include <dhcpsrv/testutils/lib_load_test_fixture.h>
#include <process/daemon.h>
#include <gtest/gtest.h>

using namespace isc::config;
using namespace isc::data;
using namespace isc::dhcp;
using namespace isc::process;
using namespace std;

namespace {

class CbCmdsLibLoadTest : public isc::test::LibLoadTest {
public:
    CbCmdsLibLoadTest() : LibLoadTest(LIBDHCP_CB_CMDS_SO) {
    }

    virtual ~CbCmdsLibLoadTest() {
        unloadLibraries();
    }
};

TEST_F(CbCmdsLibLoadTest, validLoad4) {
    validDaemonTest("kea-dhcp4");
}

TEST_F(CbCmdsLibLoadTest, validLoad6) {
    validDaemonTest("kea-dhcp6", AF_INET6);
}

TEST_F(CbCmdsLibLoadTest, invalidDaemonLoad) {
    invalidDaemonTest("kea-dhcp-ddns");
    invalidDaemonTest("bogus");
}

TEST_F(CbCmdsLibLoadTest, commands4) {
    CfgMgr::instance().setFamily(AF_INET);
    Daemon::setProcName("kea-dhcp4");
    addLibrary(LIBDHCP_CB_CMDS_SO, ConstElementPtr());
    ASSERT_TRUE(loadLibraries());

    ConstElementPtr rsp = CommandMgr::instance().processCommand(
        Element::fromJSON("{ \"command\": \"list-commands\" }"));
    ASSERT_TRUE(rsp);
    ASSERT_TRUE(rsp->get("arguments"));
    string args_txt = rsp->get("arguments")->str();
    EXPECT_NE(string::npos, args_txt.find("remote-server4-set"));
    EXPECT_NE(string::npos, args_txt.find("remote-subnet4-list"));
    EXPECT_EQ(string::npos, args_txt.find("remote-server6-set"));
    EXPECT_EQ(string::npos, args_txt.find("remote-subnet6-list"));
}

TEST_F(CbCmdsLibLoadTest, commands6) {
    CfgMgr::instance().setFamily(AF_INET6);
    Daemon::setProcName("kea-dhcp6");
    addLibrary(LIBDHCP_CB_CMDS_SO, ConstElementPtr());
    ASSERT_TRUE(loadLibraries());

    ConstElementPtr rsp = CommandMgr::instance().processCommand(
        Element::fromJSON("{ \"command\": \"list-commands\" }"));
    ASSERT_TRUE(rsp);
    ASSERT_TRUE(rsp->get("arguments"));
    string args_txt = rsp->get("arguments")->str();
    EXPECT_NE(string::npos, args_txt.find("remote-server6-set"));
    EXPECT_NE(string::npos, args_txt.find("remote-subnet6-list"));
    EXPECT_EQ(string::npos, args_txt.find("remote-server4-set"));
    EXPECT_EQ(string::npos, args_txt.find("remote-subnet4-list"));
}

} // end of anonymous namespace
