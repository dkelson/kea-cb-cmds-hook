// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <cb_cmds.h>
#include <cc/command_interpreter.h>
#include <config/command_mgr.h>
#include <database/server.h>
#include <database/server_selector.h>
#include <database/database_connection.h>
#include <dhcpsrv/cfgmgr.h>
#include <dhcpsrv/config_backend_dhcp4_mgr.h>
#include <dhcpsrv/config_backend_dhcp6_mgr.h>
#include <dhcpsrv/testutils/test_config_backend_dhcp4.h>
#include <dhcpsrv/testutils/test_config_backend_dhcp6.h>
#include <hooks/callout_manager.h>
#include <hooks/callout_handle.h>
#include <hooks/hooks_manager.h>
#include <hooks/server_hooks.h>
#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>

using namespace isc::cb_cmds;
using namespace isc::config;
using namespace isc::data;
using namespace isc::db;
using namespace isc::dhcp;
using namespace isc::dhcp::test;
using namespace isc::hooks;
using namespace std;

namespace {

ConstElementPtr
parse(const string& json) {
    return (Element::fromJSON(json));
}

string
quote(const string& value) {
    return ("\"" + value + "\"");
}

string
commandJson(const string& command, const string& args = "") {
    ostringstream s;
    s << "{ \"command\": " << quote(command);
    if (!args.empty()) {
        s << ", \"arguments\": " << args;
    }
    s << " }";
    return (s.str());
}

class CbCmdsTest : public ::testing::Test {
public:
    void SetUp() override {
        CfgMgr::instance().clear();
        CfgMgr::instance().setFamily(AF_INET);
        CommandMgr::instance();
        ServerHooks::getServerHooks().reset();
        callout_manager_.reset(new CalloutManager(1));
        ConfigBackendDHCPv4Mgr::create();
        ConfigBackendDHCPv6Mgr::create();
        ConfigBackendDHCPv4Mgr::instance().registerBackendFactory("memfile",
            [](const DatabaseConnection::ParameterMap& params) -> ConfigBackendDHCPv4Ptr {
                return (TestConfigBackendDHCPv4Ptr(new TestConfigBackendDHCPv4(params)));
            });
        ConfigBackendDHCPv6Mgr::instance().registerBackendFactory("memfile",
            [](const DatabaseConnection::ParameterMap& params) -> ConfigBackendDHCPv6Ptr {
                return (TestConfigBackendDHCPv6Ptr(new TestConfigBackendDHCPv6(params)));
            });
        ConfigBackendDHCPv4Mgr::instance().addBackend("type=memfile host=alpha port=1001");
        ConfigBackendDHCPv6Mgr::instance().addBackend("type=memfile host=alpha port=1001");
    }

    void TearDown() override {
        ConfigBackendDHCPv4Mgr::instance().delAllBackends();
        ConfigBackendDHCPv6Mgr::instance().delAllBackends();
        CfgMgr::instance().clear();
    }

    ConstElementPtr run(const string& command, const string& args = "") {
        CalloutHandlePtr handle(new CalloutHandle(callout_manager_));
        ConstElementPtr cmd = parse(commandJson(command, args));
        handle->setArgument("command", cmd);
        cmds_.handleCommand(*handle);

        ConstElementPtr response;
        handle->getArgument("response", response);
        EXPECT_TRUE(response);
        return (response);
    }

    ConstElementPtr runJson(const string& json) {
        CalloutHandlePtr handle(new CalloutHandle(callout_manager_));
        ConstElementPtr cmd = parse(json);
        handle->setArgument("command", cmd);
        cmds_.handleCommand(*handle);

        ConstElementPtr response;
        handle->getArgument("response", response);
        EXPECT_TRUE(response);
        return (response);
    }

    void expectResult(const ConstElementPtr& response, const int result) {
        ASSERT_TRUE(response);
        ASSERT_TRUE(response->get("result")) << response->str();
        EXPECT_EQ(result, response->get("result")->intValue()) << response->str();
    }

    ConstElementPtr expectSuccess(const ConstElementPtr& response) {
        expectResult(response, CONTROL_RESULT_SUCCESS);
        return (response->get("arguments") ? response->get("arguments") :
                Element::createMap());
    }

    ConstElementPtr expectEmpty(const ConstElementPtr& response) {
        expectResult(response, CONTROL_RESULT_EMPTY);
        if (!response->get("arguments")) {
            ADD_FAILURE() << response->str();
            return (Element::createMap());
        }
        return (response->get("arguments"));
    }

    void expectError(const ConstElementPtr& response) {
        expectResult(response, CONTROL_RESULT_ERROR);
        ASSERT_TRUE(response->get("text")) << response->str();
    }

    string tags(const string& tags_json = "\"alpha\"") const {
        return ("\"server-tags\": [ " + tags_json + " ]");
    }

    string remote(const string& host = "alpha", const int port = 1001) const {
        static_cast<void>(port);
        ostringstream s;
        s << "\"remote\": { \"host\": " << quote(host) << " }";
        return (s.str());
    }

    string withRemote(const string& body, const string& host = "alpha",
                      const int port = 1001) const {
        return ("{ " + remote(host, port) + (body.empty() ? "" : ", " + body) + " }");
    }

    string withTags(const string& body, const string& tags_json = "\"alpha\"") const {
        return ("{ " + tags(tags_json) + (body.empty() ? "" : ", " + body) + " }");
    }

private:
    CbCmds cmds_;
    boost::shared_ptr<CalloutManager> callout_manager_;
};

TEST_F(CbCmdsTest, unsupportedCommandReturnsError) {
    expectError(runJson("{ \"command\": \"remote-unknown4\" }"));
}

TEST_F(CbCmdsTest, apiDescriptorsAreAcceptedByHandler) {
    const string api_dir = string(TOP_SOURCE_DIR) + "/src/share/api";
    DIR* dir = opendir(api_dir.c_str());
    ASSERT_TRUE(dir) << "unable to open " << api_dir;

    set<string> commands;
    while (dirent* entry = readdir(dir)) {
        string file_name(entry->d_name);
        if ((file_name.find("remote-") != 0) ||
            (file_name.size() < 6) ||
            (file_name.substr(file_name.size() - 5) != ".json")) {
            continue;
        }

        ConstElementPtr descriptor = Element::fromJSONFile(api_dir + "/" + file_name);
        if (!descriptor->get("hook") ||
            (descriptor->get("hook")->stringValue() != "cb_cmds")) {
            continue;
        }

        ASSERT_TRUE(descriptor->get("name")) << file_name;
        commands.insert(descriptor->get("name")->stringValue());
    }
    closedir(dir);

    ASSERT_FALSE(commands.empty());
    set<string> registered_commands;
    for (auto const& command : commandNames()) {
        registered_commands.insert(command);
    }
    EXPECT_EQ(commands, registered_commands);

    for (auto const& command : commands) {
        SCOPED_TRACE(command);
        ConstElementPtr response = run(command);
        ASSERT_TRUE(response->get("text")) << response->str();
        EXPECT_EQ(string::npos, response->get("text")->stringValue().find("unsupported command"))
            << response->str();
    }
}

TEST_F(CbCmdsTest, serverCommandsUseRemoteSelector) {
    ConfigBackendDHCPv4Mgr::instance().addBackend("type=memfile host=beta port=1002");

    expectSuccess(run("remote-server4-set", withRemote("\"servers\": [ { "
        "\"server-tag\": \"alpha\", \"description\": \"a\" } ]")));
    expectSuccess(run("remote-server4-set", withRemote("\"servers\": [ { "
        "\"server-tag\": \"beta\", \"description\": \"b\" } ]", "beta", 1002)));

    ConstElementPtr args = expectSuccess(run("remote-server4-get-all",
                                             "{ \"remote\": { \"host\": \"alpha\" } }"));
    ASSERT_TRUE(args->get("servers")) << args->str();
    EXPECT_EQ(1u, args->get("servers")->size()) << args->str();
    EXPECT_EQ("alpha", args->get("servers")->get(0)->get("server-tag")->stringValue());
    EXPECT_EQ(1, args->get("count")->intValue());

    args = expectSuccess(run("remote-server4-get-all",
                             "{ \"remote\": { \"host\": \"beta\" } }"));
    ASSERT_TRUE(args->get("servers")) << args->str();
    EXPECT_EQ(1u, args->get("servers")->size()) << args->str();
    EXPECT_EQ("beta", args->get("servers")->get(0)->get("server-tag")->stringValue());

    args = expectSuccess(run("remote-server4-get",
                             "{ \"remote\": { \"host\": \"alpha\" }, "
                             "\"server-tag\": \"alpha\" }"));
    ASSERT_TRUE(args->get("servers")) << args->str();
    ASSERT_EQ(1u, args->get("servers")->size()) << args->str();
    EXPECT_EQ("alpha", args->get("servers")->get(0)->get("server-tag")->stringValue());

    args = expectSuccess(run("remote-server4-del",
                             "{ \"remote\": { \"host\": \"alpha\" }, "
                             "\"server-tag\": \"alpha\" }"));
    EXPECT_EQ(1, args->get("count")->intValue());
}

TEST_F(CbCmdsTest, serverCommandsRejectServerTagsAndValidateTagNames) {
    expectError(run("remote-server4-set", "{ \"server-tags\": [ \"alpha\" ], "
                    "\"servers\": [ { \"server-tag\": \"srv\" } ] }"));
    expectError(run("remote-server4-set", "{ \"servers\": [ { \"server-tag\": \"all\" } ] }"));
    expectError(run("remote-server4-set", "{ \"servers\": [ { \"server-tag\": \"any\" } ] }"));

    string long_tag(257, 'a');
    expectError(run("remote-server4-set", "{ \"servers\": [ { \"server-tag\": "
                    + quote(long_tag) + " } ] }"));
}

TEST_F(CbCmdsTest, globalParameterCommandsRoundTripAndSelectServerTags) {
    expectSuccess(run("remote-global-parameter4-set",
                      withTags("\"parameters\": { \"valid-lifetime\": 4000 }")));
    expectSuccess(run("remote-global-parameter4-set",
                      withTags("\"parameters\": { \"valid-lifetime\": 5000 }", "\"beta\"")));

    ConstElementPtr args = expectSuccess(run("remote-global-parameter4-get",
                                             withTags("\"parameters\": [ \"valid-lifetime\" ]")));
    ASSERT_TRUE(args->get("parameters")) << args->str();
    EXPECT_EQ(4000, args->get("parameters")->get("valid-lifetime")->intValue());
    ASSERT_TRUE(args->get("parameters")->get("metadata")) << args->str();
    ASSERT_TRUE(args->get("parameters")->get("metadata")->get("server-tags")) << args->str();

    args = expectSuccess(run("remote-global-parameter4-get",
                             withTags("\"parameters\": [ \"valid-lifetime\" ]", "\"beta\"")));
    EXPECT_EQ(5000, args->get("parameters")->get("valid-lifetime")->intValue());

    expectError(run("remote-global-parameter4-get-all",
                    withTags("", "\"alpha\", \"beta\"")));
    args = expectSuccess(run("remote-global-parameter4-get-all", withTags("")));
    EXPECT_EQ(1, args->get("count")->intValue()) << args->str();
    ASSERT_TRUE(args->get("parameters")->get("valid-lifetime")) << args->str();

    args = expectSuccess(run("remote-global-parameter4-del",
                             withTags("\"parameters\": [ \"valid-lifetime\" ]")));
    EXPECT_EQ(1, args->get("count")->intValue());

    expectEmpty(run("remote-global-parameter4-get",
                    withTags("\"parameters\": [ \"valid-lifetime\" ]")));
}

TEST_F(CbCmdsTest, globalParameterCommandsValidateServerTags) {
    expectError(run("remote-global-parameter4-set",
                    "{ \"parameters\": { \"valid-lifetime\": 4000 } }"));
    expectError(run("remote-global-parameter4-set",
                    "{ \"server-tags\": [ null ], \"parameters\": { \"valid-lifetime\": 4000 } }"));
    expectError(run("remote-global-parameter4-set",
                    "{ \"server-tags\": [ \"alpha\", \"beta\" ], "
                    "\"parameters\": { \"valid-lifetime\": 4000 } }"));
}

TEST_F(CbCmdsTest, subnetCommandsRoundTripAndListByTags) {
    expectSuccess(run("remote-subnet4-set", withTags("\"subnets\": [ { "
        "\"id\": 10, \"subnet\": \"192.0.2.0/24\", \"shared-network-name\": null, "
        "\"pools\": [ { "
        "\"pool\": \"192.0.2.10-192.0.2.20\" } ] } ]")));
    expectSuccess(run("remote-subnet4-set", withTags("\"subnets\": [ { "
        "\"id\": 20, \"subnet\": \"198.51.100.0/24\", "
        "\"shared-network-name\": null } ]", "\"beta\"")));

    ConstElementPtr args = expectSuccess(run("remote-subnet4-list", withTags("")));
    ASSERT_TRUE(args->get("subnets")) << args->str();
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    EXPECT_EQ(10, args->get("subnets")->get(0)->get("id")->intValue());
    ASSERT_TRUE(args->get("subnets")->get(0)->get("metadata")) << args->str();

    args = expectSuccess(run("remote-subnet4-get-by-prefix",
                             "{ \"subnets\": [ { \"subnet\": \"192.0.2.0/24\" } ] }"));
    ASSERT_TRUE(args->get("subnets")) << args->str();
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    EXPECT_EQ(10, args->get("subnets")->get(0)->get("id")->intValue());

    args = expectSuccess(run("remote-subnet4-get-by-id",
                             "{ \"subnets\": [ { \"id\": 10 } ] }"));
    ASSERT_TRUE(args->get("subnets")) << args->str();
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    EXPECT_EQ("192.0.2.0/24", args->get("subnets")->get(0)->get("subnet")->stringValue());

    args = expectSuccess(run("remote-subnet4-del-by-id",
                             "{ \"subnets\": [ { \"id\": 10 } ] }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-subnet4-get-by-id", "{ \"subnets\": [ { \"id\": 10 } ] }"));

    args = expectSuccess(run("remote-subnet4-del-by-prefix",
                             "{ \"subnets\": [ { \"subnet\": \"198.51.100.0/24\" } ] }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-subnet4-get-by-prefix",
                    "{ \"subnets\": [ { \"subnet\": \"198.51.100.0/24\" } ] }"));
}

TEST_F(CbCmdsTest, sharedNetworkCommandsRoundTrip) {
    expectSuccess(run("remote-network4-set", withTags("\"shared-networks\": [ { "
        "\"name\": \"floor1\", \"valid-lifetime\": 3600 } ]")));
    expectSuccess(run("remote-subnet4-set", withTags("\"subnets\": [ { "
        "\"id\": 30, \"subnet\": \"203.0.113.0/24\", "
        "\"shared-network-name\": \"floor1\" } ]")));

    ConstElementPtr args = expectSuccess(run("remote-network4-list", withTags("")));
    ASSERT_TRUE(args->get("shared-networks")) << args->str();
    ASSERT_EQ(1u, args->get("shared-networks")->size()) << args->str();
    EXPECT_EQ("floor1", args->get("shared-networks")->get(0)->get("name")->stringValue());

    args = expectSuccess(run("remote-network4-get",
                             "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], "
                             "\"subnets-include\": \"full\" }"));
    ASSERT_TRUE(args->get("shared-networks")) << args->str();
    ASSERT_EQ(1u, args->get("shared-networks")->size()) << args->str();
    EXPECT_EQ("floor1", args->get("shared-networks")->get(0)->get("name")->stringValue());
    ASSERT_TRUE(args->get("shared-networks")->get(0)->get("metadata")) << args->str();
    ASSERT_TRUE(args->get("shared-networks")->get(0)->get("subnet4")) << args->str();
    EXPECT_EQ(1u, args->get("shared-networks")->get(0)->get("subnet4")->size())
        << args->str();

    args = expectSuccess(run("remote-network4-del",
                             "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], "
                             "\"subnets-action\": \"keep\" }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-network4-get",
                    "{ \"shared-networks\": [ { \"name\": \"floor1\" } ] }"));
    args = expectSuccess(run("remote-subnet4-get-by-id",
                             "{ \"subnets\": [ { \"id\": 30 } ] }"));
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();

    expectSuccess(run("remote-network4-set", withTags("\"shared-networks\": [ { "
        "\"name\": \"floor2\" } ]")));
    expectSuccess(run("remote-subnet4-set", withTags("\"subnets\": [ { "
        "\"id\": 31, \"subnet\": \"203.0.114.0/24\", "
        "\"shared-network-name\": \"floor2\" } ]")));
    args = expectSuccess(run("remote-network4-del",
                             "{ \"shared-networks\": [ { \"name\": \"floor2\" } ], "
                             "\"subnets-action\": \"delete\" }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-subnet4-get-by-id",
                    "{ \"subnets\": [ { \"id\": 31 } ] }"));
}

TEST_F(CbCmdsTest, optionDefCommandsRoundTrip) {
    expectSuccess(run("remote-option-def4-set", withTags("\"option-defs\": [ { "
        "\"name\": \"foo\", \"code\": 224, \"space\": \"dhcp4\", "
        "\"type\": \"string\", \"array\": false, \"record-types\": \"\", "
        "\"encapsulate\": \"\" } ]")));

    ConstElementPtr args = expectSuccess(run("remote-option-def4-get",
        withTags("\"option-defs\": [ { \"code\": 224, \"space\": \"dhcp4\" } ]")));
    ASSERT_TRUE(args->get("option-defs")) << args->str();
    ASSERT_EQ(1u, args->get("option-defs")->size()) << args->str();
    EXPECT_EQ("foo", args->get("option-defs")->get(0)->get("name")->stringValue());

    args = expectSuccess(run("remote-option-def4-get-all", withTags("")));
    ASSERT_TRUE(args->get("option-defs")) << args->str();
    EXPECT_EQ(1u, args->get("option-defs")->size()) << args->str();

    args = expectSuccess(run("remote-option-def4-del",
        withTags("\"option-defs\": [ { \"code\": 224, \"space\": \"dhcp4\" } ]")));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-option-def4-get",
        withTags("\"option-defs\": [ { \"code\": 224, \"space\": \"dhcp4\" } ]")));
}

TEST_F(CbCmdsTest, globalOptionCommandsRoundTrip) {
    expectSuccess(run("remote-option4-global-set", withTags("\"options\": [ { "
        "\"name\": \"domain-name-servers\", \"code\": 6, \"space\": \"dhcp4\", "
        "\"data\": \"192.0.2.1\" } ]")));

    ConstElementPtr args = expectSuccess(run("remote-option4-global-get",
        withTags("\"options\": [ { \"code\": 6, \"space\": \"dhcp4\" } ]")));
    ASSERT_TRUE(args->get("options")) << args->str();
    ASSERT_EQ(1u, args->get("options")->size()) << args->str();
    EXPECT_EQ(6, args->get("options")->get(0)->get("code")->intValue());
    EXPECT_FALSE(args->get("count")) << args->str();

    args = expectSuccess(run("remote-option4-global-get-all", withTags("")));
    ASSERT_TRUE(args->get("options")) << args->str();
    EXPECT_EQ(1u, args->get("options")->size()) << args->str();
    ASSERT_TRUE(args->get("count")) << args->str();

    args = expectSuccess(run("remote-option4-global-del",
        withTags("\"options\": [ { \"code\": 6, \"space\": \"dhcp4\" } ]")));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-option4-global-get",
        withTags("\"options\": [ { \"code\": 6, \"space\": \"dhcp4\" } ]")));
}

TEST_F(CbCmdsTest, scopedOptionCommandsRoundTrip) {
    expectSuccess(run("remote-network4-set", withTags("\"shared-networks\": [ { "
        "\"name\": \"floor1\" } ]")));
    expectSuccess(run("remote-subnet4-set", withTags("\"subnets\": [ { "
        "\"id\": 10, \"subnet\": \"192.0.2.0/24\", \"shared-network-name\": null, "
        "\"pools\": [ { "
        "\"pool\": \"192.0.2.10-192.0.2.20\" } ] } ]")));

    const string opt = "\"options\": [ { \"name\": \"domain-name\", \"code\": 15, "
                       "\"space\": \"dhcp4\", \"data\": \"example.com\" } ]";
    expectSuccess(run("remote-option4-network-set",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], " + opt + " }"));
    expectSuccess(run("remote-option4-subnet-set",
        "{ \"subnets\": [ { \"id\": 10 } ], " + opt + " }"));
    expectSuccess(run("remote-option4-pool-set",
        "{ \"pools\": [ { \"pool\": \"192.0.2.10-192.0.2.20\" } ], " + opt + " }"));

    EXPECT_EQ(1, expectSuccess(run("remote-option4-network-del",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option4-subnet-del",
        "{ \"subnets\": [ { \"id\": 10 } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option4-pool-del",
        "{ \"pools\": [ { \"pool\": \"192.0.2.10-192.0.2.20\" } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"))->get("count")->intValue());
}

TEST_F(CbCmdsTest, classCommandsRoundTrip) {
    expectSuccess(run("remote-class4-set",
        withTags("\"client-classes\": [ { \"name\": \"gold\", "
                 "\"test\": \"member('KNOWN')\" } ]", "\"alpha\", \"beta\"")));

    ConstElementPtr args = expectSuccess(run("remote-class4-get",
        "{ \"client-classes\": [ { \"name\": \"gold\" } ] }"));
    ASSERT_TRUE(args->get("client-classes")) << args->str();
    ASSERT_EQ(1u, args->get("client-classes")->size()) << args->str();
    EXPECT_EQ("gold", args->get("client-classes")->get(0)->get("name")->stringValue());

    args = expectSuccess(run("remote-class4-get-all", withTags("")));
    ASSERT_TRUE(args->get("client-classes")) << args->str();
    EXPECT_EQ(1u, args->get("client-classes")->size()) << args->str();

    args = expectSuccess(run("remote-class4-del",
        "{ \"client-classes\": [ { \"name\": \"gold\" } ] }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-class4-get",
        "{ \"client-classes\": [ { \"name\": \"gold\" } ] }"));
}

TEST_F(CbCmdsTest, v6SubnetNetworkClassAndOptionsHonorServerTags) {
    expectSuccess(run("remote-server6-set",
        "{ \"servers\": [ { \"server-tag\": \"alpha\", \"description\": \"v6\" } ] }"));
    ConstElementPtr args = expectSuccess(run("remote-server6-get",
                                             "{ \"server-tag\": \"alpha\" }"));
    ASSERT_TRUE(args->get("servers")) << args->str();
    ASSERT_EQ(1u, args->get("servers")->size()) << args->str();
    EXPECT_EQ("alpha", args->get("servers")->get(0)->get("server-tag")->stringValue());
    args = expectSuccess(run("remote-server6-get-all", "{}"));
    ASSERT_TRUE(args->get("servers")) << args->str();
    EXPECT_EQ(1, args->get("count")->intValue());

    expectSuccess(run("remote-global-parameter6-set",
                      withTags("\"parameters\": { \"preferred-lifetime\": 3000 }")));
    args = expectSuccess(run("remote-global-parameter6-get",
                             withTags("\"parameters\": [ \"preferred-lifetime\" ]")));
    ASSERT_TRUE(args->get("parameters")) << args->str();
    EXPECT_EQ(3000, args->get("parameters")->get("preferred-lifetime")->intValue());
    args = expectSuccess(run("remote-global-parameter6-get-all", withTags("")));
    ASSERT_TRUE(args->get("parameters")) << args->str();
    EXPECT_EQ(1, args->get("count")->intValue());

    expectSuccess(run("remote-network6-set", withTags("\"shared-networks\": [ { "
        "\"name\": \"v6net\" } ]")));
    expectSuccess(run("remote-subnet6-set", withTags("\"subnets\": [ { "
        "\"id\": 60, \"subnet\": \"2001:db8:1::/64\", "
        "\"shared-network-name\": null, \"pools\": [ { "
        "\"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], \"pd-pools\": [ { "
        "\"prefix\": \"2001:db8:2::\", \"prefix-len\": 64, "
        "\"delegated-len\": 80 } ] } ]")));
    expectSuccess(run("remote-class6-set", withTags("\"client-classes\": [ { "
        "\"name\": \"v6gold\", \"test\": \"member('KNOWN')\" } ]")));
    expectSuccess(run("remote-option6-global-set", withTags("\"options\": [ { "
        "\"name\": \"dns-servers\", \"code\": 23, \"space\": \"dhcp6\", "
        "\"data\": \"2001:db8::1\" } ]")));

    args = expectSuccess(run("remote-network6-list", withTags("")));
    ASSERT_EQ(1u, args->get("shared-networks")->size()) << args->str();
    EXPECT_EQ("v6net", args->get("shared-networks")->get(0)->get("name")->stringValue());
    args = expectSuccess(run("remote-network6-get",
                             "{ \"shared-networks\": [ { \"name\": \"v6net\" } ] }"));
    ASSERT_EQ(1u, args->get("shared-networks")->size()) << args->str();
    EXPECT_EQ("v6net", args->get("shared-networks")->get(0)->get("name")->stringValue());

    expectSuccess(run("remote-network6-set", withTags("\"shared-networks\": [ { "
        "\"name\": \"v6delete\" } ]")));
    expectSuccess(run("remote-subnet6-set", withTags("\"subnets\": [ { "
        "\"id\": 62, \"subnet\": \"2001:db8:4::/64\", "
        "\"shared-network-name\": \"v6delete\" } ]")));
    args = expectSuccess(run("remote-network6-del",
                             "{ \"shared-networks\": [ { \"name\": \"v6delete\" } ], "
                             "\"subnets-action\": \"delete\" }"));
    EXPECT_EQ(1, args->get("count")->intValue());
    expectEmpty(run("remote-subnet6-get-by-id",
                    "{ \"subnets\": [ { \"id\": 62 } ] }"));

    args = expectSuccess(run("remote-subnet6-list", withTags("")));
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    EXPECT_EQ(60, args->get("subnets")->get(0)->get("id")->intValue());
    args = expectSuccess(run("remote-subnet6-get-by-id",
                             "{ \"subnets\": [ { \"id\": 60 } ] }"));
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    args = expectSuccess(run("remote-subnet6-get-by-prefix",
                             "{ \"subnets\": [ { \"subnet\": \"2001:db8:1::/64\" } ] }"));
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();

    args = expectSuccess(run("remote-class6-get-all", withTags("")));
    ASSERT_EQ(1u, args->get("client-classes")->size()) << args->str();
    EXPECT_EQ("v6gold", args->get("client-classes")->get(0)->get("name")->stringValue());
    args = expectSuccess(run("remote-class6-get",
                             "{ \"client-classes\": [ { \"name\": \"v6gold\" } ] }"));
    ASSERT_EQ(1u, args->get("client-classes")->size()) << args->str();

    args = expectSuccess(run("remote-option6-global-get-all", withTags("")));
    ASSERT_TRUE(args->get("options")) << args->str();
    ASSERT_EQ(1u, args->get("options")->size()) << args->str();
    args = expectSuccess(run("remote-option6-global-get",
        withTags("\"options\": [ { \"code\": 23, \"space\": \"dhcp6\" } ]")));
    ASSERT_EQ(1u, args->get("options")->size()) << args->str();
    EXPECT_FALSE(args->get("count")) << args->str();

    expectSuccess(run("remote-option-def6-set", withTags("\"option-defs\": [ { "
        "\"name\": \"v6foo\", \"code\": 100, \"space\": \"dhcp6\", "
        "\"type\": \"string\", \"array\": false, \"record-types\": \"\", "
        "\"encapsulate\": \"\" } ]")));
    args = expectSuccess(run("remote-option-def6-get",
        withTags("\"option-defs\": [ { \"code\": 100, \"space\": \"dhcp6\" } ]")));
    ASSERT_EQ(1u, args->get("option-defs")->size()) << args->str();
    args = expectSuccess(run("remote-option-def6-get-all", withTags("")));
    ASSERT_EQ(1u, args->get("option-defs")->size()) << args->str();

    const string opt = "\"options\": [ { \"name\": \"bootfile-url\", \"code\": 59, "
                       "\"space\": \"dhcp6\", \"data\": \"http://example.test/boot\" } ]";
    expectSuccess(run("remote-option6-network-set",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], " + opt + " }"));
    expectSuccess(run("remote-option6-subnet-set",
        "{ \"subnets\": [ { \"id\": 60 } ], " + opt + " }"));
    expectSuccess(run("remote-option6-pool-set",
        "{ \"pools\": [ { \"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], " + opt + " }"));
    expectSuccess(run("remote-option6-pd-pool-set",
        "{ \"pd-pools\": [ { \"prefix\": \"2001:db8:2::\", \"prefix-len\": 64 } ], "
        + opt + " }"));

    EXPECT_EQ(1, expectSuccess(run("remote-option6-network-del",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option6-subnet-del",
        "{ \"subnets\": [ { \"id\": 60 } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option6-pool-del",
        "{ \"pools\": [ { \"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option6-pd-pool-del",
        "{ \"pd-pools\": [ { \"prefix\": \"2001:db8:2::\", \"prefix-len\": 64 } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"))->get("count")->intValue());

    EXPECT_EQ(1, expectSuccess(run("remote-option-def6-del",
        withTags("\"option-defs\": [ { \"code\": 100, \"space\": \"dhcp6\" } ]")))->
        get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-option6-global-del",
        withTags("\"options\": [ { \"code\": 23, \"space\": \"dhcp6\" } ]")))->
        get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-class6-del",
        "{ \"client-classes\": [ { \"name\": \"v6gold\" } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-subnet6-del-by-prefix",
        "{ \"subnets\": [ { \"subnet\": \"2001:db8:1::/64\" } ] }"))->
        get("count")->intValue());
    expectSuccess(run("remote-subnet6-set", withTags("\"subnets\": [ { "
        "\"id\": 61, \"subnet\": \"2001:db8:3::/64\", "
        "\"shared-network-name\": null } ]")));
    EXPECT_EQ(1, expectSuccess(run("remote-subnet6-del-by-id",
        "{ \"subnets\": [ { \"id\": 61 } ] }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-network6-del",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], "
        "\"subnets-action\": \"keep\" }"))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-global-parameter6-del",
        withTags("\"parameters\": [ \"preferred-lifetime\" ]")))->get("count")->intValue());
    EXPECT_EQ(1, expectSuccess(run("remote-server6-del",
        "{ \"server-tag\": \"alpha\" }"))->get("count")->intValue());
}

TEST_F(CbCmdsTest, listCommandsSupportNullForUnassignedObjects) {
    expectSuccess(run("remote-subnet4-set", "{ \"server-tags\": [ \"alpha\" ], "
        "\"subnets\": [ { \"id\": 10, \"subnet\": \"192.0.2.0/24\", "
        "\"shared-network-name\": null } ] }"));
    // Directly store an unassigned object through the backend API to verify the
    // documented [ null ] selector.
    Subnet4Ptr unassigned(new Subnet4(isc::asiolink::IOAddress("198.51.100.0"),
                                      24, 1, 2, 3, 20));
    ConfigBackendDHCPv4Mgr::instance().getPool()->createUpdateSubnet4(
        BackendSelector::UNSPEC(), ServerSelector::UNASSIGNED(), unassigned);

    ConstElementPtr args = expectSuccess(run("remote-subnet4-list",
                                             "{ \"server-tags\": [ null ] }"));
    ASSERT_TRUE(args->get("subnets")) << args->str();
    ASSERT_EQ(1u, args->get("subnets")->size()) << args->str();
    EXPECT_EQ(20, args->get("subnets")->get(0)->get("id")->intValue());
}

TEST_F(CbCmdsTest, validationErrorsCoverMalformedArguments) {
    expectError(runJson("{ \"command\": 123 }"));
    expectError(runJson("{ \"command\": \"remote-server4-get\", \"arguments\": [] }"));
    expectError(run("remote-subnet4-set", withTags("\"subnets\": []")));
    expectError(run("remote-subnet4-set", withTags("\"subnets\": [ {}, {} ]")));
    expectError(run("remote-subnet4-set", withTags("\"subnets\": [ { "
                    "\"id\": 1, \"subnet\": \"bad-prefix\" } ]")));
    expectError(run("remote-option4-global-get", withTags("\"options\": [ { "
                    "\"code\": 70000, \"space\": \"dhcp4\" } ]")));
    expectError(run("remote-option4-network-set", "{ \"server-tags\": [ \"alpha\" ], "
                    "\"shared-networks\": [ { \"name\": \"n\" } ], "
                    "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\", "
                    "\"data\": \"example.com\" } ] }"));
    expectError(run("remote-subnet4-list", "{}"));
    expectError(run("remote-network4-list", "{}"));
    expectError(run("remote-global-parameter4-get-all",
                    withTags("", "\"alpha\", \"alpha\"")));
    expectError(run("remote-option-def4-get-all",
                    withTags("", "\"alpha\", \"beta\"")));
}

} // end of anonymous namespace
