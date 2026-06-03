// Copyright (C) 2026 kea-cb-cmds-hook authors
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <config.h>

#include <cb_cmds_compat_config.h>
#include <cb_cmds.h>

#include <cc/command_interpreter.h>
#include <cc/server_tag.h>
#include <cc/stamped_value.h>
#include <database/backend_selector.h>
#include <database/server.h>
#include <database/server_selector.h>
#include <dhcpsrv/cfg_option.h>
#include <dhcpsrv/cfg_option_def.h>
#include <dhcpsrv/cfgmgr.h>
#include <dhcpsrv/config_backend_dhcp4_mgr.h>
#include <dhcpsrv/config_backend_dhcp6_mgr.h>
#include <dhcpsrv/parsers/client_class_def_parser.h>
#include <dhcpsrv/parsers/dhcp_parsers.h>
#include <dhcpsrv/parsers/option_data_parser.h>
#include <dhcpsrv/parsers/shared_network_parser.h>
#include <dhcpsrv/shared_network.h>
#include <dhcpsrv/subnet.h>
#include <exceptions/exceptions.h>
#include <util/multi_threading_mgr.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace isc::asiolink;
using namespace isc::config;
using namespace isc::data;
using namespace isc::db;
using namespace isc::dhcp;
using namespace isc::hooks;
using namespace std;

namespace isc {
namespace cb_cmds {
namespace {

typedef void (*CommandProcessor)(const string&, const ConstElementPtr&, ConstElementPtr&);

struct CommandSpec {
    string name_;
    uint16_t family_;
    CommandProcessor processor_;
};

void process4(const string& cmd, const ConstElementPtr& args, ConstElementPtr& response);
void process6(const string& cmd, const ConstElementPtr& args, ConstElementPtr& response);

string
commandName(const ConstElementPtr& command) {
    if (!command || (command->getType() != Element::map) ||
        !command->get("command") ||
        (command->get("command")->getType() != Element::string)) {
        isc_throw(BadValue, "command missing or invalid");
    }
    return (command->get("command")->stringValue());
}

ConstElementPtr
argumentsOrEmpty(const ConstElementPtr& command) {
    ConstElementPtr args;
    static_cast<void>(parseCommand(args, command));
    if (!args) {
        return (Element::createMap());
    }
    if (args->getType() != Element::map) {
        isc_throw(BadValue, "arguments must be a map");
    }
    return (args);
}

BackendSelector
backendSelector(const ConstElementPtr& args) {
    ConstElementPtr remote = args->get("remote");
    if (!remote) {
        return (BackendSelector::UNSPEC());
    }
    if (remote->getType() != Element::map) {
        isc_throw(BadValue, "'remote' must be a map");
    }
    return (BackendSelector(remote));
}

ServerSelector
serverSelector(const ConstElementPtr& args, const bool required = true,
               const bool allow_null = false, const bool allow_multiple = true) {
    ConstElementPtr tags = args->get("server-tags");
    if (!tags) {
        if (required) {
            isc_throw(BadValue, "missing mandatory 'server-tags' argument");
        }
        return (ServerSelector::ALL());
    }
    if (tags->getType() != Element::list) {
        isc_throw(BadValue, "'server-tags' must be a list");
    }
    if (tags->empty()) {
        isc_throw(BadValue, "'server-tags' must not be empty");
    }

    bool has_null = false;
    set<string> selected;
    for (size_t i = 0; i < tags->size(); ++i) {
        ConstElementPtr tag = tags->get(i);
        if (tag->getType() == Element::null) {
            has_null = true;
            continue;
        }
        if (tag->getType() != Element::string) {
            isc_throw(BadValue, "'server-tags' entries must be strings or null");
        }
        data::ServerTag server_tag(tag->stringValue());
        if (!selected.insert(server_tag.get()).second) {
            isc_throw(BadValue, "duplicate server tag '" << server_tag.get() << "'");
        }
    }
    if (has_null) {
        if (!allow_null || (tags->size() != 1)) {
            isc_throw(BadValue, "null server tag is not allowed in this command");
        }
        return (ServerSelector::UNASSIGNED());
    }
    if (selected.empty()) {
        isc_throw(BadValue, "'server-tags' must specify at least one tag");
    }
    if (!allow_multiple && (tags->size() != 1)) {
        isc_throw(BadValue, "'server-tags' must contain exactly one tag");
    }
    if (selected.size() == 1) {
        const string tag = *selected.begin();
        if (tag == data::ServerTag::ALL) {
            return (ServerSelector::ALL());
        }
        return (ServerSelector::ONE(tag));
    }
    return (ServerSelector::MULTIPLE(selected));
}

void
forbidServerTags(const ConstElementPtr& args) {
    if (args->get("server-tags")) {
        isc_throw(BadValue, "'server-tags' argument is not allowed for this command");
    }
}

string
mandatoryString(const ConstElementPtr& args, const string& name) {
    ConstElementPtr value = args->get(name);
    if (!value || (value->getType() != Element::string)) {
        isc_throw(BadValue, "missing or invalid '" << name << "' string argument");
    }
    return (value->stringValue());
}

string
mandatoryServerTag(const ConstElementPtr& args) {
    string tag = mandatoryString(args, "server-tag");
    data::ServerTag server_tag(tag);
    if ((server_tag.get() == data::ServerTag::ALL) ||
        (server_tag.get() == "any")) {
        isc_throw(BadValue, "reserved server tag '" << tag << "' is not allowed");
    }
    return (tag);
}

uint16_t
mandatoryUint16(const ConstElementPtr& args, const string& name) {
    ConstElementPtr value = args->get(name);
    if (!value || (value->getType() != Element::integer) ||
        (value->intValue() < 0) || (value->intValue() > 65535)) {
        isc_throw(BadValue, "missing or invalid '" << name << "' uint16 argument");
    }
    return (static_cast<uint16_t>(value->intValue()));
}

SubnetID
mandatorySubnetID(const ConstElementPtr& args) {
    ConstElementPtr value = args->get("id");
    if (!value) {
        value = args->get("subnet-id");
    }
    if (!value || (value->getType() != Element::integer) || (value->intValue() < 0)) {
        isc_throw(BadValue, "missing or invalid subnet id argument");
    }
    return (static_cast<SubnetID>(value->intValue()));
}

ConstElementPtr
singleListItem(const ConstElementPtr& args, const string& list_name) {
    ConstElementPtr list = args->get(list_name);
    if (!list || (list->getType() != Element::list)) {
        isc_throw(BadValue, "missing or invalid '" << list_name << "' list argument");
    }
    if (list->size() != 1) {
        isc_throw(BadValue, "'" << list_name << "' must contain exactly one item");
    }
    if (list->get(0)->getType() != Element::map) {
        isc_throw(BadValue, "'" << list_name << "' item must be a map");
    }
    return (list->get(0));
}

ConstElementPtr
singleListItemAny(const ConstElementPtr& args, const vector<string>& list_names) {
    for (auto const& name : list_names) {
        if (args->get(name)) {
            return (singleListItem(args, name));
        }
    }
    isc_throw(BadValue, "missing required list argument");
}

ConstElementPtr
serverKeyArgs(const ConstElementPtr& args) {
    return (singleListItem(args, "servers"));
}

ElementPtr
answerArgs(const string& key, const ElementPtr& value, const uint64_t count) {
    ElementPtr args = Element::createMap();
    if (!key.empty() && value) {
        args->set(key, value);
    }
    args->set("count", Element::create(static_cast<int64_t>(count)));
    return (args);
}

ElementPtr
answerArgsNoCount(const string& key, const ElementPtr& value) {
    ElementPtr args = Element::createMap();
    if (!key.empty() && value) {
        args->set(key, value);
    }
    return (args);
}

ElementPtr
subnetSetArgs(const ConstSubnetPtr& subnet) {
    ElementPtr args = Element::createMap();
    args->set("id", Element::create(static_cast<int64_t>(subnet->getID())));
    args->set("subnet", Element::create(subnet->toText()));
    return (args);
}

ElementPtr
serverSetArgs(const ServerPtr& server) {
    ElementPtr list = Element::createList();
    list->add(server->toElement());
    ElementPtr args = Element::createMap();
    args->set("servers", list);
    return (args);
}

ElementPtr
classSetArgs(const ClientClassDefPtr& cc) {
    ElementPtr list = Element::createList();
    ElementPtr item = Element::createMap();
    item->set("name", Element::create(cc->getName()));
    list->add(item);
    ElementPtr args = Element::createMap();
    args->set("client-classes", list);
    return (args);
}

ElementPtr
optionSetArgs(const OptionDescriptorPtr& desc) {
    ElementPtr list = Element::createList();
    ElementPtr item = Element::createMap();
    item->set("code", Element::create(static_cast<int64_t>(desc->option_->getType())));
    item->set("space", Element::create(desc->space_name_));
    if (!desc->client_classes_.empty()) {
        ElementPtr classes = Element::createList();
        for (auto const& client_class : desc->client_classes_) {
            classes->add(Element::create(client_class));
        }
        item->set("client-classes", classes);
    }
    list->add(item);
    ElementPtr args = Element::createMap();
    args->set("options", list);
    return (args);
}

ConstElementPtr
requireSharedNetworkName(const ConstElementPtr& subnet) {
    if (!subnet->contains("shared-network-name")) {
        isc_throw(BadValue, "missing mandatory 'shared-network-name' argument");
    }
    ConstElementPtr name = subnet->get("shared-network-name");
    if ((name->getType() != Element::string) && (name->getType() != Element::null)) {
        isc_throw(BadValue, "'shared-network-name' must be a string or null");
    }
    return (name);
}

Subnet4Ptr
parseSubnet4(const ConstElementPtr& item) {
    ConstElementPtr shared_network_name = requireSharedNetworkName(item);
    ElementPtr parser_item = isc::data::copy(item);
    parser_item->remove("shared-network-name");

    Subnet4ConfigParser parser(false);
    auto subnet = parser.parse(parser_item);
    subnet->setSharedNetworkName(shared_network_name->getType() == Element::null ?
                                 "" : shared_network_name->stringValue());
    return (subnet);
}

Subnet6Ptr
parseSubnet6(const ConstElementPtr& item) {
    ConstElementPtr shared_network_name = requireSharedNetworkName(item);
    ElementPtr parser_item = isc::data::copy(item);
    parser_item->remove("shared-network-name");

    Subnet6ConfigParser parser(false);
    auto subnet = parser.parse(parser_item);
    subnet->setSharedNetworkName(shared_network_name->getType() == Element::null ?
                                 "" : shared_network_name->stringValue());
    return (subnet);
}

string
optionalString(const ConstElementPtr& args, const string& name,
               const string& default_value) {
    ConstElementPtr value = args->get(name);
    if (!value) {
        return (default_value);
    }
    if (value->getType() != Element::string) {
        isc_throw(BadValue, "'" << name << "' must be a string");
    }
    return (value->stringValue());
}

bool
includeSharedNetworkSubnets(const ConstElementPtr& args) {
    string action = optionalString(args, "subnets-include", "no");
    if ((action != "full") && (action != "no")) {
        isc_throw(BadValue, "'subnets-include' must be 'full' or 'no'");
    }
    return (action == "full");
}

bool
deleteSharedNetworkSubnets(const ConstElementPtr& args) {
    string action = optionalString(args, "subnets-action", "keep");
    if ((action != "keep") && (action != "delete")) {
        isc_throw(BadValue, "'subnets-action' must be 'keep' or 'delete'");
    }
    return (action == "delete");
}

template<typename T>
void
setMetadata(const ElementPtr& map, const T& obj) {
    if (map && obj) {
        map->set("metadata", obj->getMetadata());
    }
}

ElementPtr
optionElement(const OptionDescriptorPtr& desc) {
    ElementPtr list = Element::createList();
    if (desc && desc->option_) {
        CfgOption cfg;
        cfg.add(*desc, desc->space_name_.empty() ? desc->option_->getEncapsulatedSpace() :
                desc->space_name_);
        list = cfg.toElementWithMetadata(true, CfgOptionDefPtr());
    }
    return (list);
}

ElementPtr
optionContainerElement(const OptionContainer& container) {
    CfgOption cfg;
    for (auto const& desc : container) {
        cfg.add(desc, desc.space_name_.empty() ? DHCP4_OPTION_SPACE : desc.space_name_);
    }
    return (cfg.toElementWithMetadata(true, CfgOptionDefPtr()));
}

OptionDescriptorPtr
parseOption(const ConstElementPtr& args, const uint16_t family) {
    ConstElementPtr item = singleListItemAny(args, { "options", "option-data" });
    OptionDataParser parser(family, CfgMgr::instance().getCurrentCfg()->getCfgOptionDef());
    auto parsed = parser.parse(item);
    OptionDescriptorPtr desc(new OptionDescriptor(parsed.first));
    desc->space_name_ = parsed.second;
    return (desc);
}

OptionDefinitionPtr
parseOptionDef(const ConstElementPtr& args, const uint16_t family) {
    ConstElementPtr item = singleListItemAny(args, { "option-defs", "option-def" });
    ElementPtr parser_item = isc::data::copy(item);
    if (!parser_item->get("array")) {
        parser_item->set("array", Element::create(false));
    }
    if (!parser_item->get("record-types")) {
        parser_item->set("record-types", Element::create(string("")));
    }
    if (!parser_item->get("encapsulate")) {
        parser_item->set("encapsulate", Element::create(string("")));
    }
    OptionDefParser parser(family);
    return (parser.parse(parser_item));
}

ClientClassDefPtr
parseClass(const ConstElementPtr& args, const uint16_t family) {
    ConstElementPtr item = singleListItemAny(args, { "client-class", "client-classes" });
    ClientClassDictionaryPtr dictionary(new ClientClassDictionary());
    ClientClassDefParser parser;
    parser.parse(dictionary, item, family, false);
    ConstElementPtr name = item->get("name");
    if (!name || (name->getType() != Element::string)) {
        isc_throw(BadValue, "missing or invalid client class name");
    }
    return (dictionary->findClass(name->stringValue()));
}

ElementPtr
classesElement(const ClientClassDictionary& dictionary) {
    ElementPtr list = Element::createList();
    auto const& classes = dictionary.getClasses();
    for (auto const& c : *classes) {
        ElementPtr e = c->toElement();
        setMetadata(e, c);
        list->add(e);
    }
    return (list);
}

#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
ClientClassesPtr
clientClassesArg(const ConstElementPtr& args) {
    ConstElementPtr classes = args->get("client-classes");
    if (!classes) {
        return (ClientClassesPtr());
    }
    if (classes->getType() != Element::list) {
        isc_throw(BadValue, "'client-classes' must be a list");
    }
    ClientClassesPtr result(new ClientClasses());
    for (size_t i = 0; i < classes->size(); ++i) {
        if (classes->get(i)->getType() != Element::string) {
            isc_throw(BadValue, "'client-classes' entries must be strings");
        }
        result->insert(classes->get(i)->stringValue());
    }
    return (result);
}
#else
void
rejectClientClassesArg(const ConstElementPtr& args) {
    if (args->get("client-classes")) {
        isc_throw(BadValue, "'client-classes' option keys require Kea 3.1 or later headers");
    }
}
#endif

uint64_t
deleteOptionDef4(ConfigBackendPoolDHCPv4& pool, const BackendSelector& backend,
                 const ServerSelector& selector, const uint16_t code,
                 const string& space, const bool force) {
#if CB_CMDS_HAVE_KEA_OPTION_DEF_DELETE_FORCE
    return (pool.deleteOptionDef4(backend, selector, code, space, force));
#else
    if (force) {
        isc_throw(BadValue, "'force' option definition delete requires Kea 3.1 or later headers");
    }
    return (pool.deleteOptionDef4(backend, selector, code, space));
#endif
}

uint64_t
deleteOptionDef6(ConfigBackendPoolDHCPv6& pool, const BackendSelector& backend,
                 const ServerSelector& selector, const uint16_t code,
                 const string& space, const bool force) {
#if CB_CMDS_HAVE_KEA_OPTION_DEF_DELETE_FORCE
    return (pool.deleteOptionDef6(backend, selector, code, space, force));
#else
    if (force) {
        isc_throw(BadValue, "'force' option definition delete requires Kea 3.1 or later headers");
    }
    return (pool.deleteOptionDef6(backend, selector, code, space));
#endif
}

ConstElementPtr
optionKeyArgs(const ConstElementPtr& args) {
    if (args->get("options")) {
        return (singleListItem(args, "options"));
    }
    if (args->get("option-data")) {
        return (singleListItem(args, "option-data"));
    }
    if (args->get("option-defs")) {
        return (singleListItem(args, "option-defs"));
    }
    if (args->get("option-def")) {
        return (singleListItem(args, "option-def"));
    }
    return (args);
}

ConstElementPtr
subnetKeyArgs(const ConstElementPtr& args) {
    return (args->get("subnets") ? singleListItem(args, "subnets") : args);
}

ConstElementPtr
networkKeyArgs(const ConstElementPtr& args) {
    return (args->get("shared-networks") ? singleListItem(args, "shared-networks") : args);
}

ConstElementPtr
poolKeyArgs(const ConstElementPtr& args) {
    return (args->get("pools") ? singleListItem(args, "pools") : args);
}

ConstElementPtr
pdPoolKeyArgs(const ConstElementPtr& args) {
    return (args->get("pd-pools") ? singleListItem(args, "pd-pools") : args);
}

pair<IOAddress, IOAddress>
poolRange(const ConstElementPtr& args) {
    string text = mandatoryString(args, "pool");
    auto dash = text.find('-');
    if (dash == string::npos) {
        isc_throw(BadValue, "'pool' must be in start-end form");
    }
    return (make_pair(IOAddress(text.substr(0, dash)),
                      IOAddress(text.substr(dash + 1))));
}

string
optionSpace(const ConstElementPtr& args, const uint16_t family) {
    ConstElementPtr space = args->get("space");
    if (space) {
        if (space->getType() != Element::string) {
            isc_throw(BadValue, "'space' must be a string");
        }
        return (space->stringValue());
    }
    return (family == AF_INET ? DHCP4_OPTION_SPACE : DHCP6_OPTION_SPACE);
}

ConstElementPtr
parameterMap(const ConstElementPtr& args) {
    ConstElementPtr params = args->get("parameters");
    if (!params || (params->getType() != Element::map)) {
        isc_throw(BadValue, "'parameters' must be a map");
    }
    return (params);
}

string
parameterName(const ConstElementPtr& args) {
    ConstElementPtr params = args->get("parameters");
    if (!params) {
        return (mandatoryString(args, "name"));
    }
    if ((params->getType() != Element::list) || (params->size() != 1) ||
        (params->get(0)->getType() != Element::string)) {
        isc_throw(BadValue, "'parameters' must be a list containing exactly one string");
    }
    return (params->get(0)->stringValue());
}

ElementPtr
serversList4(ConfigBackendPoolDHCPv4* pool, const BackendSelector& backend) {
    ElementPtr list = Element::createList();
    auto servers = pool->getAllServers4(backend);
    for (auto const& server : servers) {
        list->add(server->toElement());
    }
    return (list);
}

ElementPtr
serversList6(ConfigBackendPoolDHCPv6* pool, const BackendSelector& backend) {
    ElementPtr list = Element::createList();
    auto servers = pool->getAllServers6(backend);
    for (auto const& server : servers) {
        list->add(server->toElement());
    }
    return (list);
}

ElementPtr
optionDefElement(const OptionDefinitionPtr& def) {
    CfgOptionDef cfg;
    if (def) {
        cfg.add(def);
    }
    return (cfg.toElementWithMetadata(true));
}

ElementPtr
optionDefContainerElement(const OptionDefContainer& defs) {
    CfgOptionDef cfg;
    for (auto const& def : defs) {
        cfg.add(def);
    }
    return (cfg.toElementWithMetadata(true));
}

ElementPtr
globalParametersElement(const StampedValueCollection& values) {
    ElementPtr map = Element::createMap();
    for (auto const& value : values) {
        map->set(value->getName(), boost::const_pointer_cast<Element>(value->getElementValue()));
    }
    return (map);
}

void
process4(const string& cmd, const ConstElementPtr& args, ConstElementPtr& response) {
    auto pool = ConfigBackendDHCPv4Mgr::instance().getPool();
    BackendSelector backend = backendSelector(args);
    const bool is_list = (cmd.find("-list") != string::npos) ||
                         (cmd.find("-get-all") != string::npos);

    if (cmd == "remote-server4-set") {
        forbidServerTags(args);
        ConstElementPtr item = serverKeyArgs(args);
        ServerPtr server = Server::create(ServerTag(mandatoryServerTag(item)),
                                          item->get("description") ?
                                          item->get("description")->stringValue() : "");
        pool->createUpdateServer4(backend, server);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "server set", serverSetArgs(server));
    } else if (cmd == "remote-server4-get") {
        forbidServerTags(args);
        auto server = pool->getServer4(backend, ServerTag(mandatoryString(serverKeyArgs(args), "server-tag")));
        if (!server) {
            response = createAnswer(CONTROL_RESULT_EMPTY, "server not found", answerArgs("servers", Element::createList(), 0));
        } else {
            ElementPtr list = Element::createList();
            list->add(server->toElement());
            response = createAnswer(CONTROL_RESULT_SUCCESS, "server returned", answerArgs("servers", list, 1));
        }
    } else if (cmd == "remote-server4-get-all") {
        forbidServerTags(args);
        ElementPtr list = serversList4(pool.get(), backend);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "servers returned", answerArgs("servers", list, list->size()));
    } else if (cmd == "remote-server4-del") {
        forbidServerTags(args);
        uint64_t count = pool->deleteServer4(backend, ServerTag(mandatoryString(serverKeyArgs(args), "server-tag")));
        response = createAnswer(CONTROL_RESULT_SUCCESS, "server deleted", answerArgs("", ElementPtr(), count));

    } else if (cmd == "remote-global-parameter4-set") {
        ServerSelector selector = serverSelector(args, true, false, false);
        ConstElementPtr params = parameterMap(args);
        for (auto const& kv : params->mapValue()) {
            pool->createUpdateGlobalParameter4(backend, selector, StampedValue::create(kv.first, boost::const_pointer_cast<Element>(kv.second)));
        }
        response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameters set",
                                answerArgs("parameters", boost::const_pointer_cast<Element>(params), params->size()));
    } else if (cmd == "remote-global-parameter4-get") {
        ServerSelector selector = serverSelector(args, true, false, false);
        string name = parameterName(args);
        auto value = pool->getGlobalParameter4(backend, selector, name);
        if (!value) {
            response = createAnswer(CONTROL_RESULT_EMPTY, "global parameter not found", answerArgs("parameters", Element::createMap(), 0));
        } else {
            ElementPtr map = Element::createMap();
            map->set(name, boost::const_pointer_cast<Element>(value->getElementValue()));
            map->set("metadata", value->getMetadata());
            response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameter returned", answerArgs("parameters", map, 1));
        }
    } else if (cmd == "remote-global-parameter4-get-all") {
        ServerSelector selector = serverSelector(args, true, false, false);
        auto values = pool->getAllGlobalParameters4(backend, selector);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameters returned",
                                answerArgs("parameters", globalParametersElement(values), values.size()));
    } else if (cmd == "remote-global-parameter4-del") {
        ServerSelector selector = serverSelector(args, true, false, false);
        uint64_t count = pool->deleteGlobalParameter4(backend, selector, parameterName(args));
        response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameter deleted", answerArgs("", ElementPtr(), count));

    } else if (cmd == "remote-network4-set") {
        ServerSelector selector = serverSelector(args, true, false, true);
        ConstElementPtr item = singleListItem(args, "shared-networks");
        SharedNetwork4Parser parser(false);
        auto network = parser.parse(item);
        pool->createUpdateSharedNetwork4(backend, selector, network);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "shared network set");
    } else if (cmd == "remote-network4-get") {
        forbidServerTags(args);
        auto key = networkKeyArgs(args);
        auto network = pool->getSharedNetwork4(backend, ServerSelector::ANY(), mandatoryString(key, "name"));
        if (!network) {
            response = createAnswer(CONTROL_RESULT_EMPTY, "shared network not found", answerArgs("shared-networks", Element::createList(), 0));
        } else {
            ElementPtr list = Element::createList();
            ElementPtr e = network->toElement();
            if (includeSharedNetworkSubnets(args)) {
                ElementPtr subnets_element = Element::createList();
                auto subnets = pool->getSharedNetworkSubnets4(backend, ServerSelector::ANY(),
                                                              network->getName());
                for (auto const& subnet : subnets) {
                    ElementPtr subnet_element = subnet->toElement();
                    setMetadata(subnet_element, subnet);
                    subnets_element->add(subnet_element);
                }
                e->set("subnet4", subnets_element);
            }
            setMetadata(e, network);
            list->add(e);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "shared network returned", answerArgs("shared-networks", list, 1));
        }
    } else if (cmd == "remote-network4-list") {
        ServerSelector selector = serverSelector(args, true, true, true);
        auto networks = pool->getAllSharedNetworks4(backend, selector);
        ElementPtr list = Element::createList();
        for (auto const& network : networks) {
            ElementPtr e = Element::createMap();
            e->set("name", Element::create(network->getName()));
            setMetadata(e, network);
            list->add(e);
        }
        response = createAnswer(CONTROL_RESULT_SUCCESS, "shared networks returned", answerArgs("shared-networks", list, list->size()));
    } else if (cmd == "remote-network4-del") {
        forbidServerTags(args);
        auto key = networkKeyArgs(args);
        if (deleteSharedNetworkSubnets(args)) {
            static_cast<void>(pool->deleteSharedNetworkSubnets4(backend, ServerSelector::ANY(),
                                                                mandatoryString(key, "name")));
        }
        uint64_t count = pool->deleteSharedNetwork4(backend, ServerSelector::ANY(), mandatoryString(key, "name"));
        response = createAnswer(CONTROL_RESULT_SUCCESS, "shared network deleted", answerArgs("", ElementPtr(), count));

    } else if (cmd == "remote-subnet4-set") {
        ServerSelector selector = serverSelector(args, true, false, true);
        ConstElementPtr item = singleListItemAny(args, { "subnets", "subnet4" });
        auto subnet = parseSubnet4(item);
        pool->createUpdateSubnet4(backend, selector, subnet);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "subnet set", subnetSetArgs(subnet));
    } else if (cmd == "remote-subnet4-list") {
        ServerSelector selector = serverSelector(args, true, true, true);
        auto subnets = pool->getAllSubnets4(backend, selector);
        ElementPtr list = Element::createList();
        for (auto const& subnet : subnets) {
            ElementPtr e = Element::createMap();
            e->set("id", Element::create(static_cast<int64_t>(subnet->getID())));
            e->set("subnet", Element::create(subnet->toText()));
            e->set("shared-network-name", subnet->getSharedNetworkName().empty() ? Element::create() : Element::create(subnet->getSharedNetworkName()));
            setMetadata(e, subnet);
            list->add(e);
        }
        response = createAnswer(CONTROL_RESULT_SUCCESS, "subnets returned", answerArgs("subnets", list, list->size()));
    } else if ((cmd == "remote-subnet4-get-by-id") || (cmd == "remote-subnet4-get-by-prefix")) {
        forbidServerTags(args);
        auto key = subnetKeyArgs(args);
        Subnet4Ptr subnet = (cmd.find("id") != string::npos) ?
            pool->getSubnet4(backend, ServerSelector::ANY(), mandatorySubnetID(key)) :
            pool->getSubnet4(backend, ServerSelector::ANY(), mandatoryString(key, "subnet"));
        if (!subnet) {
            response = createAnswer(CONTROL_RESULT_EMPTY, "subnet not found", answerArgs("subnets", Element::createList(), 0));
        } else {
            ElementPtr list = Element::createList();
            ElementPtr e = subnet->toElement();
            setMetadata(e, subnet);
            list->add(e);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "subnet returned", answerArgs("subnets", list, 1));
        }
    } else if ((cmd == "remote-subnet4-del-by-id") || (cmd == "remote-subnet4-del-by-prefix")) {
        forbidServerTags(args);
        auto key = subnetKeyArgs(args);
        uint64_t count = (cmd.find("id") != string::npos) ?
            pool->deleteSubnet4(backend, ServerSelector::ANY(), mandatorySubnetID(key)) :
            pool->deleteSubnet4(backend, ServerSelector::ANY(), mandatoryString(key, "subnet"));
        response = createAnswer(CONTROL_RESULT_SUCCESS, "subnet deleted", answerArgs("", ElementPtr(), count));

    } else if (cmd.find("remote-option-def4-") == 0) {
        ServerSelector selector = serverSelector(args, true, false, false);
        if (cmd == "remote-option-def4-set") {
            pool->createUpdateOptionDef4(backend, selector, parseOptionDef(args, AF_INET));
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definition set");
        } else if (cmd == "remote-option-def4-get") {
            auto key = optionKeyArgs(args);
            auto def = pool->getOptionDef4(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET));
            if (!def) {
                response = createAnswer(CONTROL_RESULT_EMPTY, "option definition not found", answerArgs("option-defs", Element::createList(), 0));
            } else {
                ElementPtr list = Element::createList();
                list = optionDefElement(def);
                response = createAnswer(CONTROL_RESULT_SUCCESS, "option definition returned", answerArgs("option-defs", list, 1));
            }
        } else if (cmd == "remote-option-def4-get-all") {
            auto defs = pool->getAllOptionDefs4(backend, selector);
            ElementPtr list = optionDefContainerElement(defs);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definitions returned", answerArgs("option-defs", list, list->size()));
        } else {
            auto key = optionKeyArgs(args);
            uint64_t count = deleteOptionDef4(*pool, backend, selector, mandatoryUint16(key, "code"),
                                              optionSpace(key, AF_INET),
                                              args->get("force") && args->get("force")->boolValue());
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definition deleted", answerArgs("", ElementPtr(), count));
        }
    } else if (cmd.find("remote-option4-") == 0) {
        bool set = (cmd.rfind("-set") == cmd.size() - 4);
        if (cmd.find("-global-") == string::npos) {
            forbidServerTags(args);
        }
        ServerSelector selector = (cmd.find("-global-") == string::npos) ? ServerSelector::ANY() :
            serverSelector(args, true, false, false);
        if (set) {
            auto opt = parseOption(args, AF_INET);
            if (cmd.find("-global-") != string::npos) {
                pool->createUpdateOption4(backend, selector, opt);
            } else if (cmd.find("-network-") != string::npos) {
                string name = mandatoryString(networkKeyArgs(args), "name");
                if (!pool->getSharedNetwork4(backend, selector, name)) {
                    isc_throw(BadValue, "shared network '" << name << "' not found");
                }
                pool->createUpdateOption4(backend, selector, name, opt);
            } else if (cmd.find("-subnet-") != string::npos) {
                SubnetID subnet_id = mandatorySubnetID(subnetKeyArgs(args));
                if (!pool->getSubnet4(backend, selector, subnet_id)) {
                    isc_throw(BadValue, "subnet id " << subnet_id << " not found");
                }
                pool->createUpdateOption4(backend, selector, subnet_id, opt);
            } else {
                auto range = poolRange(poolKeyArgs(args));
                pool->createUpdateOption4(backend, selector, range.first, range.second, opt);
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option set", optionSetArgs(opt));
        } else if (cmd.find("-get-all") != string::npos) {
            auto opts = pool->getAllOptions4(backend, selector);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "options returned", answerArgs("options", optionContainerElement(opts), opts.size()));
        } else if (cmd.find("-get") != string::npos) {
            auto key = optionKeyArgs(args);
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
            auto opt = pool->getOption4(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET), clientClassesArg(key));
#else
            rejectClientClassesArg(key);
            auto opt = pool->getOption4(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET));
#endif
            response = createAnswer(opt ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY, opt ? "option returned" : "option not found",
                                    answerArgsNoCount("options", optionElement(opt)));
        } else {
            auto key = optionKeyArgs(args);
            uint16_t code = mandatoryUint16(key, "code");
            string space = optionSpace(key, AF_INET);
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
            ClientClassesPtr classes = clientClassesArg(key);
#else
            rejectClientClassesArg(key);
#endif
            uint64_t count = 0;
            if (cmd.find("-global-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption4(backend, selector, code, space, classes);
#else
                count = pool->deleteOption4(backend, selector, code, space);
#endif
            } else if (cmd.find("-network-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption4(backend, selector, mandatoryString(networkKeyArgs(args), "name"), code, space, classes);
#else
                count = pool->deleteOption4(backend, selector, mandatoryString(networkKeyArgs(args), "name"), code, space);
#endif
            } else if (cmd.find("-subnet-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption4(backend, selector, mandatorySubnetID(subnetKeyArgs(args)), code, space, classes);
#else
                count = pool->deleteOption4(backend, selector, mandatorySubnetID(subnetKeyArgs(args)), code, space);
#endif
            } else {
                auto range = poolRange(poolKeyArgs(args));
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption4(backend, selector, range.first, range.second, code, space, classes);
#else
                count = pool->deleteOption4(backend, selector, range.first, range.second, code, space);
#endif
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option deleted", answerArgs("", ElementPtr(), count));
        }
    } else if (cmd.find("remote-class4-") == 0) {
        ServerSelector selector = is_list || (cmd == "remote-class4-set") ?
            serverSelector(args, true, false, is_list || (cmd == "remote-class4-set")) :
            ServerSelector::ANY();
        if ((cmd == "remote-class4-get") || (cmd == "remote-class4-del")) {
            forbidServerTags(args);
        }
        if (cmd == "remote-class4-set") {
            auto cc = parseClass(args, AF_INET);
            pool->createUpdateClientClass4(backend, selector, cc,
                                           args->get("follow-class-name") ? args->get("follow-class-name")->stringValue() : "");
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client class set", classSetArgs(cc));
        } else if (cmd == "remote-class4-get") {
            auto cc = pool->getClientClass4(backend, selector, mandatoryString(singleListItemAny(args, { "client-class", "client-classes" }), "name"));
            if (!cc) {
                response = createAnswer(CONTROL_RESULT_EMPTY, "client class not found", answerArgs("client-classes", Element::createList(), 0));
            } else {
                ElementPtr list = Element::createList();
                ElementPtr e = cc->toElement();
                setMetadata(e, cc);
                list->add(e);
                response = createAnswer(CONTROL_RESULT_SUCCESS, "client class returned", answerArgs("client-classes", list, 1));
            }
        } else if (cmd == "remote-class4-get-all") {
            auto classes = pool->getAllClientClasses4(backend, selector);
            ElementPtr list = classesElement(classes);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client classes returned", answerArgs("client-classes", list, list->size()));
        } else {
            uint64_t count = pool->deleteClientClass4(backend, selector, mandatoryString(singleListItemAny(args, { "client-class", "client-classes" }), "name"));
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client class deleted", answerArgs("", ElementPtr(), count));
        }
    } else {
        isc_throw(NotImplemented, "unsupported command " << cmd);
    }
}

void
process6(const string& cmd, const ConstElementPtr& args, ConstElementPtr& response) {
    // DHCPv6 operations are mostly parallel to v4; handle v6-only option
    // overloads here and reuse the v4-shaped logic with v6 backend calls below.
    auto pool = ConfigBackendDHCPv6Mgr::instance().getPool();
    BackendSelector backend = backendSelector(args);
    const bool is_list = (cmd.find("-list") != string::npos) ||
                         (cmd.find("-get-all") != string::npos);

    if (cmd == "remote-server6-set") {
        forbidServerTags(args);
        ConstElementPtr item = serverKeyArgs(args);
        ServerPtr server = Server::create(ServerTag(mandatoryServerTag(item)),
                                          item->get("description") ?
                                          item->get("description")->stringValue() : "");
        pool->createUpdateServer6(backend, server);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "server set", serverSetArgs(server));
    } else if (cmd == "remote-server6-get") {
        forbidServerTags(args);
        auto server = pool->getServer6(backend, ServerTag(mandatoryString(serverKeyArgs(args), "server-tag")));
        ElementPtr list = Element::createList();
        if (server) {
            list->add(server->toElement());
        }
        response = createAnswer(server ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                server ? "server returned" : "server not found",
                                answerArgs("servers", list, server ? 1 : 0));
    } else if (cmd == "remote-server6-get-all") {
        forbidServerTags(args);
        ElementPtr list = serversList6(pool.get(), backend);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "servers returned", answerArgs("servers", list, list->size()));
    } else if (cmd == "remote-server6-del") {
        forbidServerTags(args);
        response = createAnswer(CONTROL_RESULT_SUCCESS, "server deleted",
                                answerArgs("", ElementPtr(), pool->deleteServer6(backend, ServerTag(mandatoryString(serverKeyArgs(args), "server-tag")))));

    } else if (cmd.find("remote-global-parameter6-") == 0) {
        ServerSelector selector = serverSelector(args, true, false, false);
        if (cmd == "remote-global-parameter6-set") {
            ConstElementPtr params = parameterMap(args);
            uint64_t count = 0;
            for (auto const& kv : params->mapValue()) {
                pool->createUpdateGlobalParameter6(backend, selector, StampedValue::create(kv.first, boost::const_pointer_cast<Element>(kv.second)));
                ++count;
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameters set",
                                    answerArgs("parameters", boost::const_pointer_cast<Element>(params), count));
        } else if (cmd == "remote-global-parameter6-get") {
            string name = parameterName(args);
            auto value = pool->getGlobalParameter6(backend, selector, name);
            ElementPtr map = Element::createMap();
            if (value) {
                map->set(name, boost::const_pointer_cast<Element>(value->getElementValue()));
                map->set("metadata", value->getMetadata());
            }
            response = createAnswer(value ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                    value ? "global parameter returned" : "global parameter not found",
                                    answerArgs("parameters", map, value ? 1 : 0));
        } else if (cmd == "remote-global-parameter6-get-all") {
            auto values = pool->getAllGlobalParameters6(backend, selector);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameters returned",
                                    answerArgs("parameters", globalParametersElement(values), values.size()));
        } else {
            response = createAnswer(CONTROL_RESULT_SUCCESS, "global parameter deleted",
                                    answerArgs("", ElementPtr(), pool->deleteGlobalParameter6(backend, selector, parameterName(args))));
        }
    } else if (cmd.find("remote-network6-") == 0) {
        ServerSelector selector = ((cmd == "remote-network6-list") || (cmd == "remote-network6-set")) ?
            serverSelector(args, true, cmd == "remote-network6-list", true) :
            ServerSelector::ANY();
        if ((cmd == "remote-network6-get") || (cmd == "remote-network6-del")) {
            forbidServerTags(args);
        }
        if (cmd == "remote-network6-set") {
            SharedNetwork6Parser parser(false);
            pool->createUpdateSharedNetwork6(backend, selector, parser.parse(singleListItem(args, "shared-networks")));
            response = createAnswer(CONTROL_RESULT_SUCCESS, "shared network set");
        } else if (cmd == "remote-network6-get") {
            auto network = pool->getSharedNetwork6(backend, selector, mandatoryString(networkKeyArgs(args), "name"));
            ElementPtr list = Element::createList();
            if (network) {
                ElementPtr e = network->toElement();
                if (includeSharedNetworkSubnets(args)) {
                    ElementPtr subnets_element = Element::createList();
                    auto subnets = pool->getSharedNetworkSubnets6(backend, ServerSelector::ANY(),
                                                                  network->getName());
                    for (auto const& subnet : subnets) {
                        ElementPtr subnet_element = subnet->toElement();
                        setMetadata(subnet_element, subnet);
                        subnets_element->add(subnet_element);
                    }
                    e->set("subnet6", subnets_element);
                }
                setMetadata(e, network);
                list->add(e);
            }
            response = createAnswer(network ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                    network ? "shared network returned" : "shared network not found",
                                    answerArgs("shared-networks", list, network ? 1 : 0));
        } else if (cmd == "remote-network6-list") {
            auto networks = pool->getAllSharedNetworks6(backend, selector);
            ElementPtr list = Element::createList();
            for (auto const& network : networks) {
                ElementPtr e = Element::createMap();
                e->set("name", Element::create(network->getName()));
                setMetadata(e, network);
                list->add(e);
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "shared networks returned", answerArgs("shared-networks", list, list->size()));
        } else {
            auto key = networkKeyArgs(args);
            if (deleteSharedNetworkSubnets(args)) {
                static_cast<void>(pool->deleteSharedNetworkSubnets6(backend, ServerSelector::ANY(),
                                                                    mandatoryString(key, "name")));
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "shared network deleted",
                                    answerArgs("", ElementPtr(), pool->deleteSharedNetwork6(backend, selector, mandatoryString(key, "name"))));
        }
    } else if (cmd.find("remote-subnet6-") == 0) {
        ServerSelector selector = ((cmd == "remote-subnet6-list") || (cmd == "remote-subnet6-set")) ?
            serverSelector(args, true, cmd == "remote-subnet6-list", true) :
            ServerSelector::ANY();
        if ((cmd == "remote-subnet6-get-by-id") || (cmd == "remote-subnet6-get-by-prefix") ||
            (cmd == "remote-subnet6-del-by-id") || (cmd == "remote-subnet6-del-by-prefix")) {
            forbidServerTags(args);
        }
        if (cmd == "remote-subnet6-set") {
            ConstElementPtr item = singleListItemAny(args, { "subnets", "subnet6" });
            auto subnet = parseSubnet6(item);
            pool->createUpdateSubnet6(backend, selector, subnet);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "subnet set", subnetSetArgs(subnet));
        } else if (cmd == "remote-subnet6-list") {
            auto subnets = pool->getAllSubnets6(backend, selector);
            ElementPtr list = Element::createList();
            for (auto const& subnet : subnets) {
                ElementPtr e = Element::createMap();
                e->set("id", Element::create(static_cast<int64_t>(subnet->getID())));
                e->set("subnet", Element::create(subnet->toText()));
                e->set("shared-network-name", subnet->getSharedNetworkName().empty() ? Element::create() : Element::create(subnet->getSharedNetworkName()));
                setMetadata(e, subnet);
                list->add(e);
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "subnets returned", answerArgs("subnets", list, list->size()));
        } else if ((cmd == "remote-subnet6-get-by-id") || (cmd == "remote-subnet6-get-by-prefix")) {
            auto key = subnetKeyArgs(args);
            Subnet6Ptr subnet = (cmd.find("id") != string::npos) ?
                pool->getSubnet6(backend, selector, mandatorySubnetID(key)) :
                pool->getSubnet6(backend, selector, mandatoryString(key, "subnet"));
            ElementPtr list = Element::createList();
            if (subnet) {
                ElementPtr e = subnet->toElement();
                setMetadata(e, subnet);
                list->add(e);
            }
            response = createAnswer(subnet ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                    subnet ? "subnet returned" : "subnet not found",
                                    answerArgs("subnets", list, subnet ? 1 : 0));
        } else {
            auto key = subnetKeyArgs(args);
            uint64_t count = (cmd.find("id") != string::npos) ?
                pool->deleteSubnet6(backend, selector, mandatorySubnetID(key)) :
                pool->deleteSubnet6(backend, selector, mandatoryString(key, "subnet"));
            response = createAnswer(CONTROL_RESULT_SUCCESS, "subnet deleted", answerArgs("", ElementPtr(), count));
        }
    } else if (cmd.find("remote-option-def6-") == 0) {
        ServerSelector selector = serverSelector(args, true, false, false);
        if (cmd == "remote-option-def6-set") {
            pool->createUpdateOptionDef6(backend, selector, parseOptionDef(args, AF_INET6));
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definition set");
        } else if (cmd == "remote-option-def6-get") {
            auto key = optionKeyArgs(args);
            auto def = pool->getOptionDef6(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET6));
            ElementPtr list = Element::createList();
            if (def) {
                list = optionDefElement(def);
            }
            response = createAnswer(def ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                    def ? "option definition returned" : "option definition not found",
                                    answerArgs("option-defs", list, def ? 1 : 0));
        } else if (cmd == "remote-option-def6-get-all") {
            auto defs = pool->getAllOptionDefs6(backend, selector);
            ElementPtr list = optionDefContainerElement(defs);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definitions returned", answerArgs("option-defs", list, list->size()));
        } else {
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option definition deleted",
                                    answerArgs("", ElementPtr(), deleteOptionDef6(*pool, backend, selector,
                                                                                  mandatoryUint16(optionKeyArgs(args), "code"),
                                                                                  optionSpace(optionKeyArgs(args), AF_INET6),
                                                                                  args->get("force") && args->get("force")->boolValue())));
        }
    } else if (cmd.find("remote-option6-") == 0) {
        bool set = (cmd.rfind("-set") == cmd.size() - 4);
        if (cmd.find("-global-") == string::npos) {
            forbidServerTags(args);
        }
        ServerSelector selector = (cmd.find("-global-") == string::npos) ? ServerSelector::ANY() :
            serverSelector(args, true, false, false);
        if (set) {
            auto opt = parseOption(args, AF_INET6);
            if (cmd.find("-global-") != string::npos) {
                pool->createUpdateOption6(backend, selector, opt);
            } else if (cmd.find("-network-") != string::npos) {
                string name = mandatoryString(networkKeyArgs(args), "name");
                if (!pool->getSharedNetwork6(backend, selector, name)) {
                    isc_throw(BadValue, "shared network '" << name << "' not found");
                }
                pool->createUpdateOption6(backend, selector, name, opt);
            } else if (cmd.find("-subnet-") != string::npos) {
                SubnetID subnet_id = mandatorySubnetID(subnetKeyArgs(args));
                if (!pool->getSubnet6(backend, selector, subnet_id)) {
                    isc_throw(BadValue, "subnet id " << subnet_id << " not found");
                }
                pool->createUpdateOption6(backend, selector, subnet_id, opt);
            } else if (cmd.find("-pd-pool-") != string::npos) {
                auto pd = pdPoolKeyArgs(args);
                pool->createUpdateOption6(backend, selector, IOAddress(mandatoryString(pd, "prefix")),
                                          static_cast<uint8_t>(mandatoryUint16(pd, "prefix-len")), opt);
            } else {
                auto range = poolRange(poolKeyArgs(args));
                pool->createUpdateOption6(backend, selector, range.first, range.second, opt);
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option set", optionSetArgs(opt));
        } else if (cmd.find("-get-all") != string::npos) {
            auto opts = pool->getAllOptions6(backend, selector);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "options returned", answerArgs("options", optionContainerElement(opts), opts.size()));
        } else if (cmd.find("-get") != string::npos) {
            auto key = optionKeyArgs(args);
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
            auto opt = pool->getOption6(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET6), clientClassesArg(key));
#else
            rejectClientClassesArg(key);
            auto opt = pool->getOption6(backend, selector, mandatoryUint16(key, "code"), optionSpace(key, AF_INET6));
#endif
            response = createAnswer(opt ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY, opt ? "option returned" : "option not found",
                                    answerArgsNoCount("options", optionElement(opt)));
        } else {
            auto key = optionKeyArgs(args);
            uint16_t code = mandatoryUint16(key, "code");
            string space = optionSpace(key, AF_INET6);
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
            ClientClassesPtr classes = clientClassesArg(key);
#else
            rejectClientClassesArg(key);
#endif
            uint64_t count = 0;
            if (cmd.find("-global-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption6(backend, selector, code, space, classes);
#else
                count = pool->deleteOption6(backend, selector, code, space);
#endif
            } else if (cmd.find("-network-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption6(backend, selector, mandatoryString(networkKeyArgs(args), "name"), code, space, classes);
#else
                count = pool->deleteOption6(backend, selector, mandatoryString(networkKeyArgs(args), "name"), code, space);
#endif
            } else if (cmd.find("-subnet-") != string::npos) {
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption6(backend, selector, mandatorySubnetID(subnetKeyArgs(args)), code, space, classes);
#else
                count = pool->deleteOption6(backend, selector, mandatorySubnetID(subnetKeyArgs(args)), code, space);
#endif
            } else if (cmd.find("-pd-pool-") != string::npos) {
                auto pd = pdPoolKeyArgs(args);
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption6(backend, selector, IOAddress(mandatoryString(pd, "prefix")),
                                            static_cast<uint8_t>(mandatoryUint16(pd, "prefix-len")), code, space, classes);
#else
                count = pool->deleteOption6(backend, selector, IOAddress(mandatoryString(pd, "prefix")),
                                            static_cast<uint8_t>(mandatoryUint16(pd, "prefix-len")), code, space);
#endif
            } else {
                auto range = poolRange(poolKeyArgs(args));
#if CB_CMDS_HAVE_KEA_OPTION_CLIENT_CLASSES
                count = pool->deleteOption6(backend, selector, range.first, range.second, code, space, classes);
#else
                count = pool->deleteOption6(backend, selector, range.first, range.second, code, space);
#endif
            }
            response = createAnswer(CONTROL_RESULT_SUCCESS, "option deleted", answerArgs("", ElementPtr(), count));
        }
    } else if (cmd.find("remote-class6-") == 0) {
        ServerSelector selector = is_list || (cmd == "remote-class6-set") ?
            serverSelector(args, true, false, is_list || (cmd == "remote-class6-set")) :
            ServerSelector::ANY();
        if ((cmd == "remote-class6-get") || (cmd == "remote-class6-del")) {
            forbidServerTags(args);
        }
        if (cmd == "remote-class6-set") {
            auto cc = parseClass(args, AF_INET6);
            pool->createUpdateClientClass6(backend, selector, cc,
                                           args->get("follow-class-name") ? args->get("follow-class-name")->stringValue() : "");
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client class set", classSetArgs(cc));
        } else if (cmd == "remote-class6-get") {
            auto cc = pool->getClientClass6(backend, selector, mandatoryString(singleListItemAny(args, { "client-class", "client-classes" }), "name"));
            ElementPtr list = Element::createList();
            if (cc) {
                ElementPtr e = cc->toElement();
                setMetadata(e, cc);
                list->add(e);
            }
            response = createAnswer(cc ? CONTROL_RESULT_SUCCESS : CONTROL_RESULT_EMPTY,
                                    cc ? "client class returned" : "client class not found",
                                    answerArgs("client-classes", list, cc ? 1 : 0));
        } else if (cmd == "remote-class6-get-all") {
            auto classes = pool->getAllClientClasses6(backend, selector);
            ElementPtr list = classesElement(classes);
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client classes returned", answerArgs("client-classes", list, list->size()));
        } else {
            response = createAnswer(CONTROL_RESULT_SUCCESS, "client class deleted",
                                    answerArgs("", ElementPtr(), pool->deleteClientClass6(backend, selector, mandatoryString(singleListItemAny(args, { "client-class", "client-classes" }), "name"))));
        }
    } else {
        isc_throw(NotImplemented, "unsupported command " << cmd);
    }
}

void
addCommands(vector<CommandSpec>& specs, const vector<string>& prefixes,
            const vector<string>& suffixes, const int family,
            const CommandProcessor processor) {
    for (auto const& prefix : prefixes) {
        for (auto const& suffix : suffixes) {
            specs.push_back({ prefix + to_string(family) + "-" + suffix,
                              static_cast<uint16_t>(family == 4 ? AF_INET : AF_INET6),
                              processor });
        }
    }
}

void
addOptionCommands(vector<CommandSpec>& specs, const int family,
                  const vector<string>& scopes,
                  const CommandProcessor processor) {
    for (auto const& scope : scopes) {
        vector<string> suffixes;
        if (scope == "global") {
            suffixes = { scope + "-set", scope + "-get", scope + "-get-all", scope + "-del" };
        } else {
            suffixes = { scope + "-set", scope + "-del" };
        }
        addCommands(specs, { "remote-option" }, suffixes, family, processor);
    }
}

vector<CommandSpec>
buildCommandSpecs() {
    vector<CommandSpec> specs;

    addCommands(specs, { "remote-server" }, { "set", "get", "get-all", "del" },
                4, process4);
    addCommands(specs, { "remote-server" }, { "set", "get", "get-all", "del" },
                6, process6);

    addCommands(specs, { "remote-global-parameter" },
                { "set", "get", "get-all", "del" }, 4, process4);
    addCommands(specs, { "remote-global-parameter" },
                { "set", "get", "get-all", "del" }, 6, process6);

    addCommands(specs, { "remote-network" }, { "set", "get", "list", "del" },
                4, process4);
    addCommands(specs, { "remote-network" }, { "set", "get", "list", "del" },
                6, process6);

    addCommands(specs, { "remote-option-def" },
                { "set", "get", "get-all", "del" }, 4, process4);
    addCommands(specs, { "remote-option-def" },
                { "set", "get", "get-all", "del" }, 6, process6);

    addOptionCommands(specs, 4, { "global", "network", "subnet", "pool" }, process4);
    addOptionCommands(specs, 6, { "global", "network", "subnet", "pool", "pd-pool" },
                      process6);

    addCommands(specs, { "remote-subnet" },
                { "set", "list", "get-by-id", "get-by-prefix",
                  "del-by-id", "del-by-prefix" }, 4, process4);
    addCommands(specs, { "remote-subnet" },
                { "set", "list", "get-by-id", "get-by-prefix",
                  "del-by-id", "del-by-prefix" }, 6, process6);

    addCommands(specs, { "remote-class" }, { "set", "get", "get-all", "del" },
                4, process4);
    addCommands(specs, { "remote-class" }, { "set", "get", "get-all", "del" },
                6, process6);

    sort(specs.begin(), specs.end(), [](const CommandSpec& a, const CommandSpec& b) {
        return (a.name_ < b.name_);
    });
    return (specs);
}

const vector<CommandSpec>&
commandSpecs() {
    static const vector<CommandSpec> specs = buildCommandSpecs();
    return (specs);
}

const CommandSpec*
findCommandSpec(const string& cmd) {
    auto const& specs = commandSpecs();
    auto found = lower_bound(specs.begin(), specs.end(), cmd,
                             [](const CommandSpec& spec, const string& name) {
                                 return (spec.name_ < name);
                             });
    if ((found == specs.end()) || (cmd != found->name_)) {
        return (nullptr);
    }
    return (&(*found));
}

} // end of anonymous namespace

vector<string>
commandNames(const uint16_t family) {
    vector<string> names;
    for (auto const& spec : commandSpecs()) {
        if (!family || (spec.family_ == family)) {
            names.push_back(spec.name_);
        }
    }
    return (names);
}

void
CbCmds::handleCommand(CalloutHandle& handle) const {
    ConstElementPtr command;
    handle.getArgument("command", command);
    ConstElementPtr response;

    try {
        string cmd = commandName(command);
        ConstElementPtr args = argumentsOrEmpty(command);
        const CommandSpec* spec = findCommandSpec(cmd);
        if (!spec) {
            isc_throw(NotImplemented, "unsupported command " << cmd);
        }
        if (cmd.find("-set") != string::npos || cmd.find("-del") != string::npos) {
            isc::util::MultiThreadingCriticalSection cs;
            spec->processor_(cmd, args, response);
        } else {
            spec->processor_(cmd, args, response);
        }
    } catch (const exception& ex) {
        response = createAnswer(CONTROL_RESULT_ERROR, ex.what());
    }

    handle.setArgument("response", response);
}

} // end of namespace isc::cb_cmds
} // end of namespace isc
