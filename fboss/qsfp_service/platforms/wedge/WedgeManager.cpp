#include "fboss/qsfp_service/platforms/wedge/WedgeManager.h"

#include <folly/gen/Base.h>

#include <folly/logging/xlog.h>
#include <fb303/ThreadCachedServiceData.h>
#include "fboss/qsfp_service/module/QsfpModule.h"
#include "fboss/qsfp_service/module/cmis/CmisModule.h"
#include "fboss/qsfp_service/module/sff/SffModule.h"
#include "fboss/qsfp_service/platforms/wedge/WedgeQsfp.h"

namespace {

constexpr int kSecAfterModuleOutOfReset = 2;

}

namespace facebook { namespace fboss {

WedgeManager::WedgeManager(std::unique_ptr<TransceiverPlatformApi> api) :
  qsfpPlatApi_(std::move(api)) {
  /* Constructor for WedgeManager class:
   * Get the TransceiverPlatformApi object from the creator of this object,
   * this object will be used for controlling the QSFP devices on board.
   * Going foward the qsfpPlatApi_ will be used to controll the QSFP devices
   * on FPGA managed platforms and the wedgeI2cBus_ will be used to control
   * the QSFP devices on I2C/CPLD managed platforms
   */
}

void WedgeManager::initTransceiverMap() {
  // If we can't get access to the USB devices, don't bother to
  // create the QSFP objects;  this is likely to be a permanent
  // error.
  try {
    wedgeI2cBus_ = getI2CBus();
  } catch (const I2cError& ex) {
    XLOG(ERR) << "failed to initialize I2C interface: " << ex.what();
    return;
  }

  // Initialize port status map for transceivers.
  for (int idx = 0; idx < getNumQsfpModules(); idx++) {
    ports_.wlock()->emplace(TransceiverID(idx), std::map<uint32_t, PortStatus>());
  }

  // Also try to load the config file here so that we have transceiver to port
  // mapping and port name recognization.
  loadConfig();

  refreshTransceivers();
}

void WedgeManager::getTransceiversInfo(std::map<int32_t, TransceiverInfo>& info,
    std::unique_ptr<std::vector<int32_t>> ids) {
  XLOG(INFO) << "Received request for getTransceiverInfo, with ids: "
             << (ids->size() > 0 ? folly::join(",", *ids) : "None");
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) |
      folly::gen::appendTo(*ids);
  }

  auto lockedTransceivers = transceivers_.rlock();
  for (const auto& i : *ids) {
    if (!isValidTransceiver(i)) {
      // If the transceiver idx is not valid,
      // just skip and continue to the next.
      continue;
    }
    TransceiverInfo trans;
    if (auto it = lockedTransceivers->find(TransceiverID(i));
         it != lockedTransceivers->end()) {
      try {
        trans = it->second->getTransceiverInfo();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << i
                  << ": Error calling getTransceiverInfo(): " << ex.what();
      }
    } else {
      trans.present_ref() = WedgeQsfp(i, wedgeI2cBus_.get()).detectTransceiver();
      trans.transceiver_ref() = TransceiverType::QSFP;
      trans.port_ref() = i;
    }
    info[i] = trans;
  }
}

void WedgeManager::getTransceiversRawDOMData(
    std::map<int32_t, RawDOMData>& info,
    std::unique_ptr<std::vector<int32_t>> ids) {
  XLOG(INFO) << "Received request for getTransceiversRawDOMData, with ids: "
             << (ids->size() > 0 ? folly::join(",", *ids) : "None");
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) |
      folly::gen::appendTo(*ids);
  }
  auto lockedTransceivers = transceivers_.rlock();
  for (const auto& i : *ids) {
    if (!isValidTransceiver(i)) {
      // If the transceiver idx is not valid,
      // just skip and continue to the next.
      continue;
    }
    RawDOMData data;
    if (auto it = lockedTransceivers->find(TransceiverID(i));
        it != lockedTransceivers->end()) {
      try {
        data = it->second->getRawDOMData();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << i
                  << ": Error calling getRawDOMData(): " << ex.what();
      }
      info[i] = data;
    }
  }
}

void WedgeManager::getTransceiversDOMDataUnion(
    std::map<int32_t, DOMDataUnion>& info,
    std::unique_ptr<std::vector<int32_t>> ids) {
  XLOG(INFO) << "Received request for getTransceiversDOMDataUnion, with ids: "
             << (ids->size() > 0 ? folly::join(",", *ids) : "None");
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) |
      folly::gen::appendTo(*ids);
  }
  auto lockedTransceivers = transceivers_.rlock();
  for (const auto& i : *ids) {
    if (!isValidTransceiver(i)) {
      // If the transceiver idx is not valid,
      // just skip and continue to the next.
      continue;
    }
    DOMDataUnion data;
    if (auto it = lockedTransceivers->find(TransceiverID(i));
        it != lockedTransceivers->end()) {
      try {
        data = it->second->getDOMDataUnion();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << i
                  << ": Error calling getDOMDataUnion(): " << ex.what();
      }
      info[i] = data;
    }
  }
}

void WedgeManager::customizeTransceiver(int32_t idx, cfg::PortSpeed speed) {
  if (!isValidTransceiver(idx)) {
    return;
  }
  auto lockedTransceivers = transceivers_.rlock();
  if (auto it = lockedTransceivers->find(TransceiverID(idx));
      it != lockedTransceivers->end()) {
    try {
      it->second->customizeTransceiver(speed);
    } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << idx
                  << ": Error calling customizeTransceiver(): " << ex.what();
    }
  }
}

void WedgeManager::syncPorts(
    std::map<int32_t, TransceiverInfo>& info,
    std::unique_ptr<std::map<int32_t, PortStatus>> ports) {
  auto groups = folly::gen::from(*ports) |
      folly::gen::filter([](const std::pair<int32_t, PortStatus>& item) {
                  return item.second.transceiverIdx_ref();
                }) |
      folly::gen::groupBy([](const std::pair<int32_t, PortStatus>& item) {
                  return *item.second.transceiverIdx_ref()
                              .value_unchecked()
                              .transceiverId_ref();
                }) |
      folly::gen::as<std::vector>();

  auto lockedTransceivers = transceivers_.rlock();
  auto lockedPorts = ports_.wlock();
  for (auto& group : groups) {
    int32_t transceiverIdx = group.key();
    auto tcvrID = TransceiverID(transceiverIdx);
    XLOG(INFO) << "Syncing ports of transceiver " << transceiverIdx;
    if (!isValidTransceiver(transceiverIdx)) {
      continue;
    }

    // Update the PortStatus map in WedgeManager.
    for (auto portStatus : group.values()) {
      lockedPorts->at(tcvrID)[portStatus.first] = portStatus.second;
    }

    if (auto it = lockedTransceivers->find(tcvrID);
        it != lockedTransceivers->end()) {
      try {
        auto transceiver = it->second.get();
        transceiver->transceiverPortsChanged(lockedPorts->at(tcvrID));
        info[transceiverIdx] = transceiver->getTransceiverInfo();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << transceiverIdx
                  << ": Error calling syncPorts(): " << ex.what();
      }
    } else {
      XLOG(ERR) << "Syncing ports to a transceiver that is not preset.";
    }
  }
}

void WedgeManager::refreshTransceivers() {
  try {
    wedgeI2cBus_->verifyBus(false);
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error calling verifyBus(): " << ex.what();
    return;
  }

  clearAllTransceiverReset();

  // Since transceivers may appear or disappear, we need to update our
  // transceiver mapping and type here.
  updateTransceiverMap();

  std::vector<folly::Future<folly::Unit>> futs;
  XLOG(INFO) << "Start refreshing all transceivers...";

  auto lockedTransceivers = transceivers_.rlock();

  for (const auto& transceiver : *lockedTransceivers) {
    XLOG(DBG3) << "Fired to refresh transceiver " << transceiver.second->getID();
    futs.push_back(transceiver.second->futureRefresh());
  }

  folly::collectAllUnsafe(futs.begin(), futs.end()).wait();
  XLOG(INFO) << "Finished refreshing all transceivers";
}

int WedgeManager::scanTransceiverPresence(
    std::unique_ptr<std::vector<int32_t>> ids) {
  // If the id list is empty, we default to scan the presence of all the
  // transcievers.
  if (ids->empty()) {
    folly::gen::range(0, getNumQsfpModules()) | folly::gen::appendTo(*ids);
  }

  std::map<int32_t, ModulePresence> presenceUpdate;
  for (auto id : *ids) {
    presenceUpdate[id] = ModulePresence::UNKNOWN;
  }

  wedgeI2cBus_->scanPresence(presenceUpdate);

  int numTransceiversUp = 0;
  for (const auto& presence : presenceUpdate) {
    if (presence.second == ModulePresence::PRESENT) {
      numTransceiversUp++;
    }
  }
  return numTransceiversUp;
}

void WedgeManager::clearAllTransceiverReset() {
  qsfpPlatApi_->clearAllTransceiverReset();
  // Required delay time between a transceiver getting out of reset and fully
  // functional.
  sleep(kSecAfterModuleOutOfReset);
}

void WedgeManager::resetTransceiver(unsigned int module) {
  qsfpPlatApi_->triggerQsfpHardReset(module);
}

std::unique_ptr<TransceiverI2CApi> WedgeManager::getI2CBus() {
  return std::make_unique<WedgeI2CBusLock>(std::make_unique<WedgeI2CBus>());
}

void WedgeManager::updateTransceiverMap() {
  auto lockedTransceivers = transceivers_.wlock();
  auto lockedPorts = ports_.rlock();
  for (int idx = 0; idx < getNumQsfpModules(); idx++) {
    auto qsfpImpl = std::make_unique<WedgeQsfp>(idx, wedgeI2cBus_.get());
    TransceiverManagementInterface transceiverManagementInterface;
    try {
      transceiverManagementInterface =
        qsfpImpl->getTransceiverManagementInterface();
    } catch (const I2cError& ex) {
      XLOG(DBG3) << "failed detecting transceiver type: " << ex.what()
                << " Transceiver " << idx << " may not be present: ";
      continue;
    }

    auto it = lockedTransceivers->find(TransceiverID(idx));
    if (it != lockedTransceivers->end()) {
      // In the case where we already have a transceiver recorded, try to check
      // whether they match the transceiver type.
      if (it->second->managementInterface() == transceiverManagementInterface) {
        // The management interface matches. Nothing needs to be done.
        continue;
      } else {
        // The management changes. Need to Delete the old module to make place
        // for the new one.
        lockedTransceivers->erase(it);
      }
    }

    // Either we don't have a transceiver here before or we had a new one since
    // the management interface changed, we want to create a new module here.
    int portsPerTransceiver =
        (portGroupMap_.size() == 0
        ? numPortsPerTransceiver()
        : portGroupMap_[idx].size());
    if (transceiverManagementInterface == TransceiverManagementInterface::CMIS)
    {
      XLOG(INFO) << "making CMIS QSFP for " << idx;
      lockedTransceivers->emplace(
          TransceiverID(idx),
          std::make_unique<CmisModule>(
              this,
              std::move(qsfpImpl),
              portsPerTransceiver));
    } else if (transceiverManagementInterface ==
               TransceiverManagementInterface::SFF) {
      XLOG(INFO) << "making Sff QSFP for " << idx;
      lockedTransceivers->emplace(
          TransceiverID(idx),
          std::make_unique<SffModule>(
              this,
              std::move(qsfpImpl),
              portsPerTransceiver));
    } else {
      XLOG(DBG3) << "Unknown Transceiver interface. Skipping idx " << idx;
      continue;
    }

    // Feed its port status to the newly constructed transceiver.
    if (auto iter = lockedPorts->find(TransceiverID(idx));
        iter != lockedPorts->end()) {
      try {
        lockedTransceivers->at(TransceiverID(idx))
                          ->transceiverPortsChanged(iter->second);
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Transceiver " << idx
                  << ": Error calling transceiverPortsChanged: " << ex.what();
      }
    }
  }
}

/* Get the i2c transaction counters from TranscieverManager base class
 * and update to fbagent. The TransceieverManager base class is inherited
 * by platform speficic Transaceiver Manager class like WedgeManager.
 * That class has the function to get the I2c transaction status.
 */
void WedgeManager::publishI2cTransactionStats() {
  // Get the i2c transaction stats from TransactionManager class (its
  // sub-class having platform specific implementation)
  auto counters = getI2cControllerStats();

  if (counters.size() == 0)
    return;

  // Populate the i2c stats per pim and per controller

  for (const I2cControllerStats& counter : counters) {
    // Publish all the counters to FbAgent

    auto statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".readTotal");
    tcData().setCounter(statName, *counter.readTotal__ref());

    statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".readFailed");
    tcData().setCounter(statName, *counter.readFailed__ref());

    statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".readBytes");
    tcData().setCounter(statName, *counter.readBytes__ref());

    statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".writeTotal");
    tcData().setCounter(statName, *counter.writeTotal__ref());

    statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".writeFailed");
    tcData().setCounter(statName, *counter.writeFailed__ref());

    statName = folly::to<std::string>(
        "qsfp.", *counter.controllerName__ref(), ".writeBytes");
    tcData().setCounter(statName, *counter.writeBytes__ref());
  }
}

}} // facebook::fboss
