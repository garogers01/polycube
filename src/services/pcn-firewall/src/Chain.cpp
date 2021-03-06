/*
 * Copyright 2017 The Polycube Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Chain.h"
#include "Firewall.h"

Chain::Chain(Firewall &parent, const ChainJsonObject &conf) : parent_(parent) {
  update(conf);
}

Chain::~Chain() {}

void Chain::update(const ChainJsonObject &conf) {
  // This method updates all the object/parameter in Chain object specified in
  // the conf JsonObject.

  if (conf.nameIsSet()) {
    name = conf.getName();
  }

  if (conf.defaultIsSet()) {
    setDefault(conf.getDefault());
  }

  if (conf.statsIsSet()) {
    for (auto &i : conf.getStats()) {
      auto id = i.getId();
      auto m = getStats(id);
      m->update(i);
    }
  }

  if (conf.ruleIsSet()) {
    for (auto &i : conf.getRule()) {
      auto id = i.getId();
      auto m = getRule(id);
      m->update(i);
    }
  }
}

ChainJsonObject Chain::toJsonObject() {
  ChainJsonObject conf;

  conf.setDefault(getDefault());

  for (auto &i : getStatsList()) {
    conf.addChainStats(i->toJsonObject());
  }

  conf.setName(getName());
  for (auto &i : getRuleList()) {
    conf.addChainRule(i->toJsonObject());
  }

  return conf;
}

ActionEnum Chain::getDefault() {
  // This method retrieves the default value.
  return this->defaultAction;
}

void Chain::setDefault(const ActionEnum &value) {
  // This method set the default value.
  if (this->defaultAction == value) {
    logger()->debug("[{0}] Default action already set. ", parent_.getName());
    return;
  }
  this->defaultAction = value;
  try {
    parent_
        .programs[std::make_pair(ModulesConstants::DEFAULTACTION,
                                 ChainNameEnum::INVALID)]
        ->reload();

  } catch (std::runtime_error re) {
    logger()->error(
        "[{0}] Can't reload the code for default action. Error: {1} ",
        parent_.getName(), re.what());
    return;
  }

  logger()->debug("[{0}] Default action set. ", parent_.getName());
}

ChainNameEnum Chain::getName() {
  // This method retrieves the name value.
  return this->name;
}

ChainAppendOutputJsonObject Chain::append(ChainAppendInputJsonObject input) {
  ChainRuleJsonObject conf;
  if (input.conntrackIsSet()) {
    conf.setConntrack(input.getConntrack());
  }
  if (input.srcIsSet()) {
    conf.setSrc(input.getSrc());
  }
  if (input.dstIsSet()) {
    conf.setDst(input.getDst());
  }
  if (input.sportIsSet()) {
    conf.setSport(input.getSport());
  }
  if (input.dportIsSet()) {
    conf.setDport(input.getDport());
  }
  if (input.tcpflagsIsSet()) {
    conf.setTcpflags(input.getTcpflags());
  }
  if (input.l4protoIsSet()) {
    conf.setL4proto(input.getL4proto());
  }
  if (input.descriptionIsSet()) {
    conf.setDescription(input.getDescription());
  }
  if (input.actionIsSet()) {
    conf.setAction(input.getAction());
  } else {
    conf.setAction(ActionEnum::DROP);
  }

  uint32_t id = rules_.size();
  conf.setId(id);
  ChainAppendOutputJsonObject result;
  addRule(id, conf);
  result.setId(id);
  return result;
}

ChainResetCountersOutputJsonObject Chain::resetCounters() {
  ChainResetCountersOutputJsonObject result;
  try {
    std::map<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *> &programs =
        parent_.programs;

    if (programs.find(std::make_pair(ModulesConstants::ACTION, name)) ==
        programs.end()) {
      throw std::runtime_error("No action loaded yet.");
    }

    auto actionProgram = dynamic_cast<Firewall::ActionLookup *>(
        programs[std::make_pair(ModulesConstants::ACTION, name)]);

    for (auto cr : rules_) {
      actionProgram->flushCounters(cr->getId());
    }

    dynamic_cast<Firewall::DefaultAction *>(
        programs[std::make_pair(ModulesConstants::DEFAULTACTION,
                                ChainNameEnum::INVALID)])
        ->flushCounters(name);

    counters_.clear();

    result.setResult(true);
  } catch (std::exception &e) {
    logger()->error("[{0}] Flushing counters error: {1} ", parent_.getName(),
                    e.what());
    result.setResult(false);
  }

  return result;
}

ChainApplyRulesOutputJsonObject Chain::applyRules() {
  ChainApplyRulesOutputJsonObject result;
  try {
    updateChain();
    result.setResult(true);
  } catch (...) {
    result.setResult(false);
  }
  return result;
}

std::shared_ptr<spdlog::logger> Chain::logger() {
  return parent_.logger();
}

uint32_t Chain::getNrRules() {
  /*
   * ChainRule::get returns only the valid rules to avoid segmentation faults
   * all around the code.
   * This methods returns the true number of rules, that can't be get from
   * ChainRule::get without
   * looking for the max id.
   */
  return rules_.size();
}

/*
 * returns only valid elements in the rules_ vector
 */
std::vector<std::shared_ptr<ChainRule>> Chain::getRealRuleList() {
  std::vector<std::shared_ptr<ChainRule>> rules;
  for (auto &rule : rules_) {
    if (rule) {
      rules.push_back(rule);
    }
  }
  return rules;
}

void Chain::updateChain() {
  logger()->info("[{0}] Starting to update the {1} chain for {2} rules...",
                 parent_.get_name(),
                 ChainJsonObject::ChainNameEnum_to_string(name), rules_.size());
  // std::lock_guard<std::mutex> lkBpf(parent_.bpfInjectMutex);
  auto start = std::chrono::high_resolution_clock::now();

  int index;
  if (name == ChainNameEnum::INGRESS) {
    index = 3 + (chainNumber * ModulesConstants::NR_MODULES);
  } else {
    index = 3 + chainNumber * ModulesConstants::NR_MODULES +
            ModulesConstants::NR_MODULES * 2;
  }

  int startingIndex = index;
  Firewall::Program *firstProgramLoaded;
  std::map<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>
      newProgramsChain;
  std::map<uint8_t, std::vector<uint64_t>> states;
  std::map<struct IpAddr, std::vector<uint64_t>> ips;
  std::map<uint16_t, std::vector<uint64_t>> ports;
  std::map<int, std::vector<uint64_t>> protocols;
  std::vector<std::vector<uint64_t>> flags;

  auto rules = getRealRuleList();

  // Looping through conntrack
  conntrack_from_rules_to_map(states, rules);
  if (!states.empty()) {
    // At least one rule requires a matching on conntrack, so it can be
    // injected.
    if (!parent_.isContrackActive()) {
      logger()->error(
          "[{0}] Conntrack is not active, please remember to activate it.",
          parent_.getName());
    }
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::CONNTRACKMATCH, name),
            new Firewall::ConntrackMatch(index, name, this->parent_)));
    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::ConntrackMatch *>(
        newProgramsChain[std::make_pair(ModulesConstants::CONNTRACKMATCH,
                                        name)])
        ->updateMap(states);

    // This check is not really needed here, it will always be the first module
    // to be injected
    if (index == startingIndex) {
      firstProgramLoaded = newProgramsChain[std::make_pair(
          ModulesConstants::CONNTRACKMATCH, name)];
    }
    ++index;
  }
  states.clear();
  // Done looping through conntrack

  // Looping through IP source
  ip_from_rules_to_map(SOURCE_TYPE, ips, rules);
  if (!ips.empty()) {
    // At least one rule requires a matching on ipsource, so inject
    // the module on the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::IPSOURCE, name),
            new Firewall::IpLookup(index, name, SOURCE_TYPE, this->parent_)));
    // If this is the first module, adjust parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded =
          newProgramsChain[std::make_pair(ModulesConstants::IPSOURCE, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::IpLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::IPSOURCE, name)])
        ->updateMap(ips);
  }
  ips.clear();
  // Done looping through IP source

  // Looping through IP destination
  ip_from_rules_to_map(DESTINATION_TYPE, ips, rules);

  if (!ips.empty()) {
    // At least one rule requires a matching on source ip, so inject the
    // module on the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::IPDESTINATION, name),
            new Firewall::IpLookup(index, name, DESTINATION_TYPE,
                                   this->parent_)));
    // If this is the first module, adjust parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded = newProgramsChain[std::make_pair(
          ModulesConstants::IPDESTINATION, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::IpLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::IPDESTINATION, name)])
        ->updateMap(ips);
  }
  ips.clear();
  // Done looping through IP destination

  // Looping through l4 protocol
  transportproto_from_rules_to_map(protocols, rules);

  if (!protocols.empty()) {
    // At least one rule requires a matching on
    // source ports, so inject the module
    // on the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::L4PROTO, name),
            new Firewall::L4ProtocolLookup(index, name, this->parent_)));

    // If this is the first module, adjust parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded =
          newProgramsChain[std::make_pair(ModulesConstants::L4PROTO, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::L4ProtocolLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::L4PROTO, name)])
        ->updateMap(protocols);
  }
  protocols.clear();
  // Done looping through l4 protocol

  // Looping through source port
  port_from_rules_to_map(SOURCE_TYPE, ports, rules);

  if (!ports.empty()) {
    // At least one rule requires a matching on  source ports,
    // so inject the  module  on the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::PORTSOURCE, name),
            new Firewall::L4PortLookup(index, name, SOURCE_TYPE,
                                       this->parent_)));

    // If this is the first module, adjust parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded =
          newProgramsChain[std::make_pair(ModulesConstants::PORTSOURCE, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::L4PortLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::PORTSOURCE, name)])
        ->updateMap(ports);
  }
  ports.clear();
  // Done looping through source port

  // Looping through destination port
  port_from_rules_to_map(DESTINATION_TYPE, ports, rules);

  if (!ports.empty()) {
    // At least one rule requires a matching on source ports,
    // so inject the module  on the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::PORTDESTINATION, name),
            new Firewall::L4PortLookup(index, name, DESTINATION_TYPE,
                                       this->parent_)));
    // If this is the first module, adjust
    // parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded = newProgramsChain[std::make_pair(
          ModulesConstants::PORTDESTINATION, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::L4PortLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::PORTDESTINATION,
                                        name)])
        ->updateMap(ports);
  }
  ports.clear();
  // Done looping through destination port

  // Looping through tcp flags
  flags_from_rules_to_map(flags, rules);

  if (!flags.empty()) {
    // At least one rule requires a matching on flags,
    // so inject the  module in the first available position
    newProgramsChain.insert(
        std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
            std::make_pair(ModulesConstants::TCPFLAGS, name),
            new Firewall::TcpFlagsLookup(index, name, this->parent_)));

    // If this is the first module, adjust parsing to forward to it.
    if (index == startingIndex) {
      firstProgramLoaded =
          newProgramsChain[std::make_pair(ModulesConstants::TCPFLAGS, name)];
    }
    ++index;

    // Now the program is loaded, populate it.
    dynamic_cast<Firewall::TcpFlagsLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::TCPFLAGS, name)])
        ->updateMap(flags);
  }
  flags.clear();

  // Done looping through tcp flags

  // Adding bitscan
  newProgramsChain.insert(
      std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
          std::make_pair(ModulesConstants::BITSCAN, name),
          new Firewall::BitScan(index, name, this->parent_)));
  // If this is the first module, adjust parsing to forward to it.
  if (index == startingIndex) {
    firstProgramLoaded =
        newProgramsChain[std::make_pair(ModulesConstants::BITSCAN, name)];
  }
  ++index;

  // Adding action taker
  newProgramsChain.insert(
      std::pair<std::pair<uint8_t, ChainNameEnum>, Firewall::Program *>(
          std::make_pair(ModulesConstants::ACTION, name),
          new Firewall::ActionLookup(index, name, this->parent_)));

  for (auto rule : rules) {
    dynamic_cast<Firewall::ActionLookup *>(
        newProgramsChain[std::make_pair(ModulesConstants::ACTION, name)])
        ->updateTableValue(rule->getId(),
                           ChainRule::ActionEnum_to_int(rule->getAction()));
  }
  // The new chain is ready. Instruct chainForwarder to switch to the new chain.
  parent_
      .programs[std::make_pair(ModulesConstants::CHAINFORWARDER,
                               ChainNameEnum::INVALID)]
      ->updateHop(1, firstProgramLoaded, name);

  parent_
      .programs[std::make_pair(ModulesConstants::CHAINFORWARDER,
                               ChainNameEnum::INVALID)]
      ->reload();

  // The parser has to be reloaded to account the new nmbr of elements
  parent_
      .programs[std::make_pair(ModulesConstants::PARSER,
                               ChainNameEnum::INVALID)]
      ->reload();

  // Unload the programs belonging to the old chain.
  for (auto it = parent_.programs.begin(); it != parent_.programs.end();) {
    if (it->first.second == name) {
      delete it->second;
      it = parent_.programs.erase(it);
    } else {
      ++it;
    }
  }

  // Copy the new program references to the main map.
  for (auto &program : newProgramsChain) {
    parent_
        .programs[std::make_pair(program.first.first, program.first.second)] =
        program.second;
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_seconds = end - start;

  // toggle chainNumberIngress
  chainNumber = (chainNumber == 0) ? 1 : 0;
  logger()->info("[{0}] Rules for the {1} chain have been updated in {2}s!",
                 parent_.get_name(),
                 ChainJsonObject::ChainNameEnum_to_string(name),
                 elapsed_seconds.count());
}

std::shared_ptr<ChainStats> Chain::getStats(const uint32_t &id) {
  if (rules_.size() < id || !rules_[id]) {
    throw std::runtime_error("There is no rule " + id);
  }

  auto &counters = counters_;

  if (counters.size() <= id || !counters[id]) {
    // Counter not initialized yet
    ChainStatsJsonObject conf;
    uint64_t pkts, bytes;
    conf.setId(id);
    ChainStats::fetchCounters(*this, id, pkts, bytes);
    conf.setPkts(pkts);
    conf.setBytes(bytes);
    if (counters.size() <= id) {
      counters.resize(id + 1);
    }
    counters[id].reset(new ChainStats(*this, conf));
  } else {
    // Counter already existed, update it
    uint64_t pkts, bytes;
    ChainStats::fetchCounters(*this, id, pkts, bytes);
    counters[id]->counter.setPkts(counters[id]->getPkts() + pkts);
    counters[id]->counter.setBytes(counters[id]->getBytes() + bytes);
  }

  return counters[id];
}

std::vector<std::shared_ptr<ChainStats>> Chain::getStatsList() {
  std::vector<std::shared_ptr<ChainStats>> vect;

  for (std::shared_ptr<ChainRule> cr : rules_) {
    if (cr) {
      vect.push_back(getStats(cr->getId()));
    }
  }

  vect.push_back(ChainStats::getDefaultActionCounters(*this));

  return vect;
}

void Chain::addStats(const uint32_t &id, const ChainStatsJsonObject &conf) {
  throw std::runtime_error("[ChainStats]: Method create not allowed.");
}

void Chain::addStatsList(const std::vector<ChainStatsJsonObject> &conf) {
  throw std::runtime_error("[ChainStats]: Method create not allowed.");
}

void Chain::replaceStats(const uint32_t &id, const ChainStatsJsonObject &conf) {
  throw std::runtime_error("[ChainStats]: Method replace not allowed.");
}

void Chain::delStats(const uint32_t &id) {
  throw std::runtime_error("[ChainStats]: Method removeEntry not allowed");
}

void Chain::delStatsList() {
  throw std::runtime_error("[ChainStats]: Method removeEntry not allowed");
}

std::shared_ptr<ChainRule> Chain::getRule(const uint32_t &id) {
  if (rules_.size() < id || !rules_[id]) {
    throw std::runtime_error("There is no rule " + id);
  }
  return rules_[id];
}

std::vector<std::shared_ptr<ChainRule>> Chain::getRuleList() {
  auto rules(getRealRuleList());

  // Adding a "stub" default rule
  ChainRuleJsonObject defaultRule;
  defaultRule.setAction(getDefault());
  defaultRule.setDescription("Default Policy");
  defaultRule.setId(0);

  rules.push_back(
      std::shared_ptr<ChainRule>(new ChainRule(*this, defaultRule)));

  return rules;
}

void Chain::addRule(const uint32_t &id, const ChainRuleJsonObject &conf) {
  auto newRule = std::make_shared<ChainRule>(*this, conf);

  // Forcing counters update
  getStatsList();

  if (newRule == nullptr) {
    // Totally useless, but it is needed to avoid the compiler making wrong
    // assumptions and reordering
    throw new std::runtime_error("I won't be thrown");

  } else if (rules_.size() <= id && newRule != nullptr) {
    rules_.resize(id + 1);
  }
  if (rules_[id]) {
    logger()->info("Rule {0} overwritten!", id);
  }

  rules_[id] = newRule;

  if (parent_.interactive_) {
    updateChain();
  }
}

void Chain::addRuleList(const std::vector<ChainRuleJsonObject> &conf) {
  for (auto &i : conf) {
    uint32_t id_ = i.getId();
    addRule(id_, i);
  }
}

void Chain::replaceRule(const uint32_t &id, const ChainRuleJsonObject &conf) {
  delRule(id);
  uint32_t id_ = conf.getId();
  addRule(id_, conf);
}

void Chain::delRule(const uint32_t &id) {
  if (rules_.size() < id || !rules_[id]) {
    throw std::runtime_error("There is no rule " + id);
  }

  // Forcing counters update
  getStatsList();

  for (uint32_t i = id; i < rules_.size() - 1; ++i) {
    rules_[i] = rules_[i + 1];
    rules_[i]->id = i;
  }

  rules_.resize(rules_.size() - 1);

  for (uint32_t i = id; i < counters_.size() - 1; ++i) {
    counters_[i] = counters_[i + 1];
    counters_[i]->counter.setId(i);
  }
  rules_.resize(counters_.size() - 1);

  if (parent_.interactive_) {
    applyRules();
  }
}

void Chain::delRuleList() {
  rules_.clear();
  counters_.clear();
  if (parent_.interactive_) {
    applyRules();
  }
}