// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <cb_cmds.h>
#include <cc/command_interpreter.h>
#include <database/backend_selector.h>
#include <database/testutils/schema.h>
#include <database/server_selector.h>
#include <dhcpsrv/cfgmgr.h>
#include <dhcpsrv/config_backend_dhcp4_mgr.h>
#include <dhcpsrv/config_backend_dhcp6_mgr.h>
#include <exceptions/exceptions.h>
#include <hooks/callout_handle.h>
#include <hooks/callout_manager.h>
#include <hooks/server_hooks.h>
#include <gtest/gtest.h>

#ifdef HAVE_MYSQL
#include <mysql_cb_dhcp4.h>
#include <mysql_cb_dhcp6.h>
#include <mysql/testutils/mysql_schema.h>
#endif

#ifdef HAVE_PGSQL
#include <pgsql_cb_dhcp4.h>
#include <pgsql_cb_dhcp6.h>
#include <pgsql/testutils/pgsql_schema.h>
#endif

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace isc::cb_cmds;
using namespace isc::config;
using namespace isc::data;
using namespace isc::db;
using namespace isc::db::test;
using namespace isc::dhcp;
using namespace isc::hooks;
using namespace std;

namespace {

bool
dbTestsEnabled() {
    const char* enabled = getenv("KEA_CB_CMDS_DB_TESTS");
    return (enabled && (string(enabled) == "1"));
}

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

string
tags(const string& body) {
    return ("{ \"server-tags\": [ \"alpha\" ]" + (body.empty() ? "" : ", " + body) + " }");
}

class CbCmdsDbTest : public ::testing::Test {
public:
    CbCmdsDbTest()
        : active_(false), cmds_(), callout_manager_() {
    }

    void SetUp() override {
        if (!dbTestsEnabled()) {
            GTEST_SKIP() << "set KEA_CB_CMDS_DB_TESTS=1 to run DB-backed cb_cmds tests";
        }

        CfgMgr::instance().clear();
        CfgMgr::instance().setFamily(AF_INET);
        ServerHooks::getServerHooks().reset();
        callout_manager_.reset(new CalloutManager(1));
        ConfigBackendDHCPv4Mgr::create();
        ConfigBackendDHCPv6Mgr::create();
        active_ = true;
    }

    void TearDown() override {
        if (active_) {
            ConfigBackendDHCPv4Mgr::destroy();
            ConfigBackendDHCPv6Mgr::destroy();
            CfgMgr::instance().clear();
            active_ = false;
        }
    }

    bool active() const {
        return (active_);
    }

    ConstElementPtr run(const string& command, const string& args = "") {
        CalloutHandlePtr handle(new CalloutHandle(callout_manager_));
        handle->setArgument("command", parse(commandJson(command, args)));
        cmds_.handleCommand(*handle);

        ConstElementPtr response;
        handle->getArgument("response", response);
        EXPECT_TRUE(response);
        return (response);
    }

    ConstElementPtr expectSuccess(const ConstElementPtr& response) {
        EXPECT_TRUE(response) << "missing response";
        EXPECT_TRUE(response->get("result")) << response->str();
        EXPECT_EQ(CONTROL_RESULT_SUCCESS, response->get("result")->intValue())
            << response->str();
        return (response->get("arguments") ? response->get("arguments") : Element::createMap());
    }

    ConstElementPtr expectEmpty(const ConstElementPtr& response) {
        EXPECT_TRUE(response) << "missing response";
        EXPECT_TRUE(response->get("result")) << response->str();
        EXPECT_EQ(CONTROL_RESULT_EMPTY, response->get("result")->intValue())
            << response->str();
        return (response->get("arguments") ? response->get("arguments") : Element::createMap());
    }

private:
    bool active_;
    CbCmds cmds_;
    boost::shared_ptr<CalloutManager> callout_manager_;
};

#ifdef HAVE_MYSQL
void
runMySqlTcpScript(const string& script_name) {
    ostringstream cmd;
    cmd << "mysql -N -B --host=127.0.0.1 "
        << "--user=keatest --password=keatest keatest < "
        << DATABASE_SCRIPTS_DIR << "/mysql/" << script_name;
    int retval = ::system(cmd.str().c_str());
    if (retval) {
        cerr << "runMySqlTcpScript failed: " << cmd.str() << endl;
        isc_throw(isc::Unexpected, "runMySqlTcpScript failed: " << cmd.str());
    }
}

struct MySqlBackend {
    static string name() {
        return ("mysql");
    }

    static BackendSelector selector() {
        return (BackendSelector(BackendSelector::Type::MYSQL));
    }

    static string connectionString() {
        return (::connectionString(MYSQL_VALID_TYPE, VALID_NAME, VALID_HOST_TCP,
                                   VALID_USER, VALID_PASSWORD));
    }

    static void createSchema() {
        runMySqlTcpScript("dhcpdb_drop.mysql");
        runMySqlTcpScript("dhcpdb_create.mysql");
    }

    static void destroySchema() {
        runMySqlTcpScript("dhcpdb_drop.mysql");
    }

    static void registerBackendTypes() {
        MySqlConfigBackendDHCPv4::registerBackendType();
        MySqlConfigBackendDHCPv6::registerBackendType();
    }
};
#endif

#ifdef HAVE_PGSQL
struct PgSqlBackend {
    static string name() {
        return ("postgresql");
    }

    static BackendSelector selector() {
        return (BackendSelector(BackendSelector::Type::POSTGRESQL));
    }

    static string connectionString() {
        return (validPgSQLConnectionString());
    }

    static void createSchema() {
        createPgSQLSchema(true, true);
    }

    static void destroySchema() {
        destroyPgSQLSchema(true, true);
    }

    static void registerBackendTypes() {
        PgSqlConfigBackendDHCPv4::registerBackendType();
        PgSqlConfigBackendDHCPv6::registerBackendType();
    }
};
#endif

template<typename Backend>
class CbCmdsDbBackendTest : public CbCmdsDbTest {
public:
    void SetUp() override {
        CbCmdsDbTest::SetUp();
        if (!this->active()) {
            return;
        }

        Backend::createSchema();
        Backend::registerBackendTypes();
        ConfigBackendDHCPv4Mgr::instance().addBackend(Backend::connectionString());
        ConfigBackendDHCPv6Mgr::instance().addBackend(Backend::connectionString());
    }

    void TearDown() override {
        if (this->active()) {
            ConfigBackendDHCPv4Mgr::destroy();
            ConfigBackendDHCPv6Mgr::destroy();
            try {
                Backend::destroySchema();
            } catch (...) {
            }
            CfgMgr::instance().clear();
        }
    }

    void reopenBackends() {
        ConfigBackendDHCPv4Mgr::destroy();
        ConfigBackendDHCPv6Mgr::destroy();
        ConfigBackendDHCPv4Mgr::create();
        ConfigBackendDHCPv6Mgr::create();
        Backend::registerBackendTypes();
        ConfigBackendDHCPv4Mgr::instance().addBackend(Backend::connectionString());
        ConfigBackendDHCPv6Mgr::instance().addBackend(Backend::connectionString());
    }
};

typedef ::testing::Types<
#ifdef HAVE_MYSQL
    MySqlBackend
#ifdef HAVE_PGSQL
    ,
#endif
#endif
#ifdef HAVE_PGSQL
    PgSqlBackend
#endif
> BackendTypes;

TYPED_TEST_SUITE(CbCmdsDbBackendTest, BackendTypes);

TYPED_TEST(CbCmdsDbBackendTest, v4CommandsPersistThroughBackend) {
    SCOPED_TRACE(TypeParam::name());

    this->expectSuccess(this->run("remote-server4-set",
        "{ \"servers\": [ { \"server-tag\": \"alpha\", \"description\": \"db\" } ] }"));
    this->expectSuccess(this->run("remote-global-parameter4-set",
        tags("\"parameters\": { \"valid-lifetime\": 4000 }")));
    this->expectSuccess(this->run("remote-network4-set",
        tags("\"shared-networks\": [ { \"name\": \"floor1\", "
             "\"valid-lifetime\": 3600 } ]")));
    this->expectSuccess(this->run("remote-subnet4-set", tags("\"subnets\": [ { "
        "\"id\": 10, \"subnet\": \"192.0.2.0/24\", "
        "\"shared-network-name\": null, \"pools\": [ { "
        "\"pool\": \"192.0.2.10-192.0.2.20\" } ] } ]")));
    this->expectSuccess(this->run("remote-subnet4-set", tags("\"subnets\": [ { "
        "\"id\": 11, \"subnet\": \"198.51.100.0/24\", "
        "\"shared-network-name\": null } ]")));
    this->expectSuccess(this->run("remote-option-def4-set", tags("\"option-defs\": [ { "
        "\"name\": \"foo\", \"code\": 224, \"space\": \"dhcp4\", "
        "\"type\": \"string\" } ]")));
    this->expectSuccess(this->run("remote-option4-global-set", tags("\"options\": [ { "
        "\"name\": \"domain-name-servers\", \"code\": 6, \"space\": \"dhcp4\", "
        "\"data\": \"192.0.2.1\" } ]")));
    this->expectSuccess(this->run("remote-class4-set", tags("\"client-classes\": [ { "
        "\"name\": \"gold\", \"test\": \"member('KNOWN')\" } ]")));

    this->reopenBackends();

    this->expectSuccess(this->run("remote-server4-get",
        "{ \"servers\": [ { \"server-tag\": \"alpha\" } ] }"));
    this->expectSuccess(this->run("remote-server4-get-all", "{}"));
    this->expectSuccess(this->run("remote-global-parameter4-get",
        tags("\"parameters\": [ \"valid-lifetime\" ]")));
    this->expectSuccess(this->run("remote-global-parameter4-get-all", tags("")));
    this->expectSuccess(this->run("remote-network4-list", tags("")));
    this->expectSuccess(this->run("remote-network4-get",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ] }"));
    this->expectSuccess(this->run("remote-subnet4-list", tags("")));
    this->expectSuccess(this->run("remote-subnet4-get-by-id",
        "{ \"subnets\": [ { \"id\": 10 } ] }"));
    this->expectSuccess(this->run("remote-subnet4-get-by-prefix",
        "{ \"subnets\": [ { \"subnet\": \"192.0.2.0/24\" } ] }"));
    this->expectSuccess(this->run("remote-option-def4-get",
        tags("\"option-defs\": [ { \"code\": 224, \"space\": \"dhcp4\" } ]")));
    this->expectSuccess(this->run("remote-option-def4-get-all", tags("")));
    this->expectSuccess(this->run("remote-option4-global-get",
        tags("\"options\": [ { \"code\": 6, \"space\": \"dhcp4\" } ]")));
    this->expectSuccess(this->run("remote-option4-global-get-all", tags("")));
    this->expectSuccess(this->run("remote-class4-get",
        "{ \"client-classes\": [ { \"name\": \"gold\" } ] }"));
    this->expectSuccess(this->run("remote-class4-get-all", tags("")));

    const string scoped_option4 = "\"options\": [ { \"name\": \"domain-name\", "
                                  "\"code\": 15, \"space\": \"dhcp4\", "
                                  "\"data\": \"example.com\" } ]";
    this->expectSuccess(this->run("remote-option4-network-set",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], " + scoped_option4 + " }"));
    this->expectSuccess(this->run("remote-option4-subnet-set",
        "{ \"subnets\": [ { \"id\": 10 } ], " + scoped_option4 + " }"));
    this->expectSuccess(this->run("remote-option4-pool-set",
        "{ \"pools\": [ { \"pool\": \"192.0.2.10-192.0.2.20\" } ], "
        + scoped_option4 + " }"));
    this->expectSuccess(this->run("remote-option4-network-del",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"));
    this->expectSuccess(this->run("remote-option4-subnet-del",
        "{ \"subnets\": [ { \"id\": 10 } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"));
    this->expectSuccess(this->run("remote-option4-pool-del",
        "{ \"pools\": [ { \"pool\": \"192.0.2.10-192.0.2.20\" } ], "
        "\"options\": [ { \"code\": 15, \"space\": \"dhcp4\" } ] }"));

    this->expectSuccess(this->run("remote-network4-set",
        tags("\"shared-networks\": [ { \"name\": \"floor-delete\" } ]")));
    this->expectSuccess(this->run("remote-subnet4-set", tags("\"subnets\": [ { "
        "\"id\": 12, \"subnet\": \"203.0.114.0/24\", "
        "\"shared-network-name\": \"floor-delete\" } ]")));
    this->expectSuccess(this->run("remote-network4-del",
        "{ \"shared-networks\": [ { \"name\": \"floor-delete\" } ], "
        "\"subnets-action\": \"delete\" }"));
    this->reopenBackends();
    this->expectEmpty(this->run("remote-subnet4-get-by-id",
        "{ \"subnets\": [ { \"id\": 12 } ] }"));

    auto pool = ConfigBackendDHCPv4Mgr::instance().getPool();
    ASSERT_TRUE(pool->getServer4(TypeParam::selector(), ServerTag("alpha")));
    ASSERT_TRUE(pool->getGlobalParameter4(TypeParam::selector(), ServerSelector::ONE("alpha"),
                                          "valid-lifetime"));
    ASSERT_TRUE(pool->getSubnet4(TypeParam::selector(), ServerSelector::ONE("alpha"), SubnetID(10)));
    ASSERT_TRUE(pool->getOptionDef4(TypeParam::selector(), ServerSelector::ONE("alpha"), 224,
                                    DHCP4_OPTION_SPACE));
    ASSERT_TRUE(pool->getOption4(TypeParam::selector(), ServerSelector::ONE("alpha"), 6,
                                 DHCP4_OPTION_SPACE));
    ASSERT_TRUE(pool->getClientClass4(TypeParam::selector(), ServerSelector::ONE("alpha"), "gold"));

    this->expectSuccess(this->run("remote-class4-del",
        "{ \"client-classes\": [ { \"name\": \"gold\" } ] }"));
    this->expectSuccess(this->run("remote-option4-global-del",
        tags("\"options\": [ { \"code\": 6, \"space\": \"dhcp4\" } ]")));
    this->expectSuccess(this->run("remote-option-def4-del",
        tags("\"option-defs\": [ { \"code\": 224, \"space\": \"dhcp4\" } ]")));
    this->expectSuccess(this->run("remote-subnet4-del-by-prefix",
        "{ \"subnets\": [ { \"subnet\": \"198.51.100.0/24\" } ] }"));
    this->expectSuccess(this->run("remote-subnet4-del-by-id",
        "{ \"subnets\": [ { \"id\": 10 } ] }"));
    this->expectSuccess(this->run("remote-network4-del",
        "{ \"shared-networks\": [ { \"name\": \"floor1\" } ], "
        "\"subnets-action\": \"keep\" }"));
    this->expectSuccess(this->run("remote-global-parameter4-del",
        tags("\"parameters\": [ \"valid-lifetime\" ]")));
    this->expectSuccess(this->run("remote-server4-del",
        "{ \"servers\": [ { \"server-tag\": \"alpha\" } ] }"));

    this->reopenBackends();
    this->expectEmpty(this->run("remote-server4-get",
        "{ \"servers\": [ { \"server-tag\": \"alpha\" } ] }"));
    this->expectEmpty(this->run("remote-global-parameter4-get",
        tags("\"parameters\": [ \"valid-lifetime\" ]")));
    this->expectEmpty(this->run("remote-subnet4-get-by-id",
        "{ \"subnets\": [ { \"id\": 10 } ] }"));
}

TYPED_TEST(CbCmdsDbBackendTest, v6CommandsPersistThroughBackend) {
    SCOPED_TRACE(TypeParam::name());

    this->expectSuccess(this->run("remote-server6-set",
        "{ \"servers\": [ { \"server-tag\": \"alpha\", \"description\": \"db\" } ] }"));
    this->expectSuccess(this->run("remote-global-parameter6-set",
        tags("\"parameters\": { \"preferred-lifetime\": 3000 }")));
    this->expectSuccess(this->run("remote-network6-set",
        tags("\"shared-networks\": [ { \"name\": \"v6net\" } ]")));
    this->expectSuccess(this->run("remote-subnet6-set", tags("\"subnets\": [ { "
        "\"id\": 60, \"subnet\": \"2001:db8:1::/64\", "
        "\"shared-network-name\": null, \"pools\": [ { "
        "\"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], \"pd-pools\": [ { "
        "\"prefix\": \"2001:db8:2::\", \"prefix-len\": 64, "
        "\"delegated-len\": 80 } ] } ]")));
    this->expectSuccess(this->run("remote-subnet6-set", tags("\"subnets\": [ { "
        "\"id\": 61, \"subnet\": \"2001:db8:3::/64\", "
        "\"shared-network-name\": null } ]")));
    this->expectSuccess(this->run("remote-option-def6-set", tags("\"option-defs\": [ { "
        "\"name\": \"v6foo\", \"code\": 100, \"space\": \"dhcp6\", "
        "\"type\": \"string\" } ]")));
    this->expectSuccess(this->run("remote-option6-global-set", tags("\"options\": [ { "
        "\"name\": \"dns-servers\", \"code\": 23, \"space\": \"dhcp6\", "
        "\"data\": \"2001:db8::1\" } ]")));
    this->expectSuccess(this->run("remote-class6-set", tags("\"client-classes\": [ { "
        "\"name\": \"v6gold\", \"test\": \"member('KNOWN')\" } ]")));

    this->reopenBackends();

    this->expectSuccess(this->run("remote-server6-get",
        "{ \"servers\": [ { \"server-tag\": \"alpha\" } ] }"));
    this->expectSuccess(this->run("remote-server6-get-all", "{}"));
    this->expectSuccess(this->run("remote-global-parameter6-get",
        tags("\"parameters\": [ \"preferred-lifetime\" ]")));
    this->expectSuccess(this->run("remote-global-parameter6-get-all", tags("")));
    this->expectSuccess(this->run("remote-network6-list", tags("")));
    this->expectSuccess(this->run("remote-network6-get",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ] }"));
    this->expectSuccess(this->run("remote-subnet6-list", tags("")));
    this->expectSuccess(this->run("remote-subnet6-get-by-id",
        "{ \"subnets\": [ { \"id\": 60 } ] }"));
    this->expectSuccess(this->run("remote-subnet6-get-by-prefix",
        "{ \"subnets\": [ { \"subnet\": \"2001:db8:1::/64\" } ] }"));
    this->expectSuccess(this->run("remote-option-def6-get",
        tags("\"option-defs\": [ { \"code\": 100, \"space\": \"dhcp6\" } ]")));
    this->expectSuccess(this->run("remote-option-def6-get-all", tags("")));
    this->expectSuccess(this->run("remote-option6-global-get",
        tags("\"options\": [ { \"code\": 23, \"space\": \"dhcp6\" } ]")));
    this->expectSuccess(this->run("remote-option6-global-get-all", tags("")));
    this->expectSuccess(this->run("remote-class6-get",
        "{ \"client-classes\": [ { \"name\": \"v6gold\" } ] }"));
    this->expectSuccess(this->run("remote-class6-get-all", tags("")));

    const string scoped_option6 = "\"options\": [ { \"name\": \"bootfile-url\", "
                                  "\"code\": 59, \"space\": \"dhcp6\", "
                                  "\"data\": \"http://example.test/boot\" } ]";
    this->expectSuccess(this->run("remote-option6-network-set",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], " + scoped_option6 + " }"));
    this->expectSuccess(this->run("remote-option6-subnet-set",
        "{ \"subnets\": [ { \"id\": 60 } ], " + scoped_option6 + " }"));
    this->expectSuccess(this->run("remote-option6-pool-set",
        "{ \"pools\": [ { \"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], "
        + scoped_option6 + " }"));
    this->expectSuccess(this->run("remote-option6-pd-pool-set",
        "{ \"pd-pools\": [ { \"prefix\": \"2001:db8:2::\", \"prefix-len\": 64 } ], "
        + scoped_option6 + " }"));
    this->expectSuccess(this->run("remote-option6-network-del",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"));
    this->expectSuccess(this->run("remote-option6-subnet-del",
        "{ \"subnets\": [ { \"id\": 60 } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"));
    this->expectSuccess(this->run("remote-option6-pool-del",
        "{ \"pools\": [ { \"pool\": \"2001:db8:1::10-2001:db8:1::20\" } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"));
    this->expectSuccess(this->run("remote-option6-pd-pool-del",
        "{ \"pd-pools\": [ { \"prefix\": \"2001:db8:2::\", \"prefix-len\": 64 } ], "
        "\"options\": [ { \"code\": 59, \"space\": \"dhcp6\" } ] }"));

    this->expectSuccess(this->run("remote-network6-set",
        tags("\"shared-networks\": [ { \"name\": \"v6-delete\" } ]")));
    this->expectSuccess(this->run("remote-subnet6-set", tags("\"subnets\": [ { "
        "\"id\": 62, \"subnet\": \"2001:db8:4::/64\", "
        "\"shared-network-name\": \"v6-delete\" } ]")));
    this->expectSuccess(this->run("remote-network6-del",
        "{ \"shared-networks\": [ { \"name\": \"v6-delete\" } ], "
        "\"subnets-action\": \"delete\" }"));
    this->reopenBackends();
    this->expectEmpty(this->run("remote-subnet6-get-by-id",
        "{ \"subnets\": [ { \"id\": 62 } ] }"));

    auto pool = ConfigBackendDHCPv6Mgr::instance().getPool();
    ASSERT_TRUE(pool->getServer6(TypeParam::selector(), ServerTag("alpha")));
    ASSERT_TRUE(pool->getGlobalParameter6(TypeParam::selector(), ServerSelector::ONE("alpha"),
                                          "preferred-lifetime"));
    ASSERT_TRUE(pool->getSubnet6(TypeParam::selector(), ServerSelector::ONE("alpha"), SubnetID(60)));
    ASSERT_TRUE(pool->getOption6(TypeParam::selector(), ServerSelector::ONE("alpha"), 23,
                                 DHCP6_OPTION_SPACE));
    ASSERT_TRUE(pool->getClientClass6(TypeParam::selector(), ServerSelector::ONE("alpha"), "v6gold"));

    this->expectSuccess(this->run("remote-class6-del",
        "{ \"client-classes\": [ { \"name\": \"v6gold\" } ] }"));
    this->expectSuccess(this->run("remote-option6-global-del",
        tags("\"options\": [ { \"code\": 23, \"space\": \"dhcp6\" } ]")));
    this->expectSuccess(this->run("remote-option-def6-del",
        tags("\"option-defs\": [ { \"code\": 100, \"space\": \"dhcp6\" } ]")));
    this->expectSuccess(this->run("remote-subnet6-del-by-prefix",
        "{ \"subnets\": [ { \"subnet\": \"2001:db8:3::/64\" } ] }"));
    this->expectSuccess(this->run("remote-subnet6-del-by-id",
        "{ \"subnets\": [ { \"id\": 60 } ] }"));
    this->expectSuccess(this->run("remote-network6-del",
        "{ \"shared-networks\": [ { \"name\": \"v6net\" } ], "
        "\"subnets-action\": \"keep\" }"));
    this->expectSuccess(this->run("remote-global-parameter6-del",
        tags("\"parameters\": [ \"preferred-lifetime\" ]")));
    this->expectSuccess(this->run("remote-server6-del",
        "{ \"servers\": [ { \"server-tag\": \"alpha\" } ] }"));
}

} // end of anonymous namespace
