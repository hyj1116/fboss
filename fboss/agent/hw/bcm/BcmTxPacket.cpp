/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmTxPacket.h"

#include "fboss/agent/hw/HwSwitchStats.h"
#include "fboss/agent/hw/bcm/BcmError.h"

extern "C" {
#include <bcm/tx.h>
}

using folly::IOBuf;
using std::unique_ptr;

namespace {

using namespace facebook::fboss;
void freeTxBuf(void* /*ptr*/, void* arg) {
  // Put the BcmTxIoBufUserData back into a unique_ptr.
  // This will delete it when we return.
  unique_ptr<facebook::fboss::BcmFreeTxBufUserData> freeTxBufUserData(
      static_cast<facebook::fboss::BcmFreeTxBufUserData*>(arg));
  const BcmPacketT& bcmPacket = freeTxBufUserData->bcmPacket;
  int rv;
  if (!bcmPacket.usePktIO) {
    bcm_pkt_t* pkt = bcmPacket.ptrUnion.pkt;
    rv = bcm_pkt_free(pkt->unit, pkt);
  } else {
#ifdef INCLUDE_PKTIO
    bcm_pktio_pkt_t* pktioPkt = bcmPacket.ptrUnion.pktioPkt;
    rv = bcm_pktio_free(pktioPkt->unit, pktioPkt);
#else
    throw FbossError("invalid PKTIO configuration.");
#endif
  }
  bcmLogError(rv, "Failed to free packet");
  freeTxBufUserData->bcmSwitch->getSwitchStats()->txPktFree();
}

inline void txCallbackImpl(int /*unit*/, bcm_pkt_t* pkt, void* cookie) {
  // Put the BcmTxCallbackUserData back into a unique_ptr.
  // This will delete it when we return.
  unique_ptr<facebook::fboss::BcmTxCallbackUserData> bcmTxCbUserData(
      static_cast<facebook::fboss::BcmTxCallbackUserData*>(cookie));
  const BcmPacketT& bcmPacket = bcmTxCbUserData->txPacket->getPkt();
  // PKTIO is synchronous.  This function is for non-PKTIO only.
  DCHECK_EQ(false, bcmPacket.usePktIO);
  DCHECK_EQ(pkt, bcmPacket.ptrUnion.pkt);

  // Now we reset the pkt buffer back to what was originally allocated
  pkt->pkt_data->data = bcmTxCbUserData->txPacket->buf()->writableBuffer();

  auto end = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end - bcmTxCbUserData->txPacket->getQueueTime());
  bcmTxCbUserData->bcmSwitch->getSwitchStats()->txSentDone(duration.count());
}
} // namespace

namespace facebook::fboss {

std::mutex& BcmTxPacket::syncPktMutex() {
  static std::mutex _syncPktMutex;
  return _syncPktMutex;
}

std::condition_variable& BcmTxPacket::syncPktCV() {
  static std::condition_variable _syncPktCV;
  return _syncPktCV;
}

bool& BcmTxPacket::syncPacketSent() {
  static bool _syncPktSent{false};
  return _syncPktSent;
}

BcmTxPacket::BcmTxPacket(int unit, uint32_t size, const BcmSwitch* bcmSwitch)
    : queued_(std::chrono::time_point<std::chrono::steady_clock>::min())
#ifdef INCLUDE_PKTIO
      ,
      unit_(unit)
#endif
{
  int rv;

  bool usePktIO = bcmSwitch->usePKTIO();
  void* bufData = nullptr;

  bcmPacket_.usePktIO = usePktIO;
  if (!usePktIO) {
    bcmPacket_.ptrUnion.pkt = nullptr;
    rv = bcm_pkt_alloc(
        unit,
        size,
        BCM_TX_CRC_APPEND | BCM_TX_ETHER,
        &(bcmPacket_.ptrUnion.pkt));
    if (BCM_FAILURE(rv)) {
      bcmSwitch->getSwitchStats()->txPktAllocErrors();
      bcmCheckError(rv, "Failed to allocate packet.");
    } else {
      bcm_pkt_t* pkt = bcmPacket_.ptrUnion.pkt;
      CHECK_NOTNULL(pkt);
      bufData = pkt->pkt_data->data;
    }
  } else {
#ifdef INCLUDE_PKTIO
    bcmPacket_.ptrUnion.pktioPkt = nullptr;
    rv = bcm_pktio_alloc(
        unit, size, BCM_PKTIO_BUF_F_TX, &(bcmPacket_.ptrUnion.pktioPkt));
    if (BCM_FAILURE(rv)) {
      bcmSwitch->getSwitchStats()->txPktAllocErrors();
      bcmCheckError(rv, "Failed to allocate packet.");
    } else {
      bcm_pktio_pkt_t* pktioPkt = bcmPacket_.ptrUnion.pktioPkt;
      CHECK_NOTNULL(pktioPkt);
      /* Advance tail pointer and set packet length in packet structure */
      rv = bcm_pktio_put(unit, pktioPkt, size, (void**)&bufData);
      bcmCheckError(rv, "Failed in bcm_pktio_put to advance tail pointer.");

      /* Get the pointer to the outgoing packet buffer */
      rv = bcm_pktio_pkt_data_get(unit, pktioPkt, (void**)&bufData, nullptr);
      bcmCheckError(rv, "Failed to get pkt data buffer.");
    }
#else
    throw FbossError("Should not reach here for SDKs without PKTIO eanbled");
#endif
  }

  DCHECK(bufData);

  auto freeTxBufUserData =
      std::make_unique<BcmFreeTxBufUserData>(bcmPacket_, bcmSwitch);

  buf_ = IOBuf::takeOwnership(
      bufData,
      size,
      freeTxBuf,
      reinterpret_cast<void*>(freeTxBufUserData.get()));

  bcmSwitch->getSwitchStats()->txPktAlloc();

  /*
   * Release the unique pointer without destroying the free buffer user
   * data. The unique pointer will be reconstructed when the IOBuf is
   * freed to free the packet buffer as well as to increment the
   * switch stats
   */
  freeTxBufUserData.release();
}

inline int BcmTxPacket::sendImpl(
    unique_ptr<BcmTxPacket> pkt,
    const BcmSwitch* bcmSwitch) noexcept {
  int rv;

  std::unique_ptr<BcmTxCallbackUserData> txCbUserData;

  bool usePktIO = pkt->bcmPacket_.usePktIO;

  if (!usePktIO) {
    bcm_pkt_t* bcmPkt = pkt->bcmPacket_.ptrUnion.pkt;
    const auto buf = pkt->buf();

    // TODO(aeckert): Setting the pkt len manually should be replaced in future
    // releases of bcm with BCM_PKT_TX_LEN_SET or bcm_flags_len_setup
    DCHECK(bcmPkt->pkt_data);
    bcmPkt->pkt_data->len = buf->length();

    // Now we also set the buffer that will be sent out to point at
    // buf->writableBuffer in case there is unused header space in the IOBuf
    bcmPkt->pkt_data->data = buf->writableData();

    pkt->queued_ = std::chrono::steady_clock::now();

    txCbUserData =
        std::make_unique<BcmTxCallbackUserData>(std::move(pkt), bcmSwitch);
    rv = bcm_tx(bcmPkt->unit, bcmPkt, txCbUserData.get());

  } else {
#ifdef INCLUDE_PKTIO
    bcm_pktio_pkt_t* pktioPkt = pkt->bcmPacket_.ptrUnion.pktioPkt;

    rv = bcm_pktio_trim(pktioPkt->unit, pktioPkt, pkt->buf_->length());
    if (BCM_FAILURE(rv)) {
      bcmLogError(rv, "failed in bcm_pktio_trim to trim the buffer.");
    } else {
      // Differnt from bcm_tx, PKTIO is synchronous.  When TX is done, it is OK
      // to release the buffer.  But for now, follow the same flow to free it
      // at the time of releasing the IOBuf to avoid duplicating the stats
      // code.
      // TODO (xiangzhu) Will further optimizing the performance, possibly
      // remove IOBuf.
      pkt->queued_ = std::chrono::steady_clock::now();
      rv = bcm_pktio_tx(pktioPkt->unit, pktioPkt);
    }
#else
    rv = BCM_E_CONFIG;
#endif
  }
  if (BCM_SUCCESS(rv)) {
    /*
     * Release the unique pointer without destroying the TxPacket object.
     * The unique pointer will be reconstructed during the Tx callback to
     * reset the packet and to increment the switch stats.
     */
    txCbUserData.release();
    bcmSwitch->getSwitchStats()->txSent();
  } else {
    bcmLogError(rv, "failed to send packet");
    if (rv == BCM_E_MEMORY) {
      bcmSwitch->getSwitchStats()->txPktAllocErrors();
    } else if (rv) {
      bcmSwitch->getSwitchStats()->txError();
    }
  }
  return rv;
}

void BcmTxPacket::txCallbackAsync(int unit, bcm_pkt_t* pkt, void* cookie) {
  txCallbackImpl(unit, pkt, cookie);
}

void BcmTxPacket::txCallbackSync(int unit, bcm_pkt_t* pkt, void* cookie) {
  txCallbackImpl(unit, pkt, cookie);
  std::lock_guard<std::mutex> lk(syncPktMutex());
  syncPacketSent() = true;
  syncPktCV().notify_one();
}

int BcmTxPacket::sendAsync(
    unique_ptr<BcmTxPacket> pkt,
    const BcmSwitch* bcmSwitch) noexcept {
  if (pkt->bcmPacket_.usePktIO) {
    /* PKTIO does not support async mode yet. Use the sync call */
    return sendSync(std::move(pkt), bcmSwitch);
  }
  bcm_pkt_t* bcmPkt = pkt->bcmPacket_.ptrUnion.pkt;
  DCHECK(bcmPkt->call_back == nullptr);
  bcmPkt->call_back = BcmTxPacket::txCallbackAsync;
  return sendImpl(std::move(pkt), bcmSwitch);
}

int BcmTxPacket::sendSync(
    unique_ptr<BcmTxPacket> pkt,
    const BcmSwitch* bcmSwitch) noexcept {
  bool usePktIO = pkt->bcmPacket_.usePktIO;

  if (!usePktIO) {
    bcm_pkt_t* bcmPkt = pkt->bcmPacket_.ptrUnion.pkt;
    DCHECK(bcmPkt->call_back == nullptr);
    bcmPkt->call_back = BcmTxPacket::txCallbackSync;
    std::lock_guard<std::mutex> lk{syncPktMutex()};
    syncPacketSent() = false;
  }
  // Give up the lock when calling sendPacketImpl. Callback
  // on packet send complete, maybe invoked immediately on
  // the same thread. Since the callback needs to acquire
  // the same lock (to mark send as done). We need to give
  // up the lock here to avoid deadlock. Further since
  // sendImpl does not update any shared data structures
  // its optimal to give up the lock anyways.
  auto rv = sendImpl(std::move(pkt), bcmSwitch);
  if (!usePktIO) {
    std::unique_lock<std::mutex> lk{syncPktMutex()};
    syncPktCV().wait(lk, [] { return syncPacketSent(); });
  }

  return rv;
}

void BcmTxPacket::setCos(uint8_t cos) noexcept {
  if (!bcmPacket_.usePktIO) {
    bcmPacket_.ptrUnion.pkt->cos = cos;
  } else {
#ifdef INCLUDE_PKTIO
    auto rv = bcm_pktio_pmd_field_set(
        unit_,
        bcmPacket_.ptrUnion.pktioPkt,
        bcmPktioPmdTypeTx,
        BCMPKT_TXPMD_COS,
        cos);
    if (BCM_FAILURE(rv)) {
      bcmLogError(rv, "failed to set COS in PKTIO PMD.");
    }
    cos_ = cos;
#endif
  }
}

} // namespace facebook::fboss
