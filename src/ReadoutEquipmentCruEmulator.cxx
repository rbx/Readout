// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "ReadoutEquipment.h"
#include "ReadoutUtils.h"
#include "readoutInfoLogger.h"
#include "RAWDataHeader.h"
#include <Common/Fifo.h>
#include <Common/Timer.h>
#include <stdlib.h>

class ReadoutEquipmentCruEmulator : public ReadoutEquipment {

public:
  ReadoutEquipmentCruEmulator(ConfigFile &cfg,
                              std::string name = "CruEmulatorReadout");
  ~ReadoutEquipmentCruEmulator();
  DataBlockContainerReference getNextBlock();
  Thread::CallbackResult prepareBlocks();

private:
  void initCounters();
  void finalCounters();

  Thread::CallbackResult populateFifoOut(); // iterative callback

  int cfgNumberOfLinks; // number of links to simulate. Will create data blocks
                        // round-robin.
  int cfgFeeId;         // FEE id to be used
  int cfgLinkId; // Link id to be used (base number - will be incremented if
                 // multiple links selected)

  const unsigned int LHCBunches = 3564; // number of bunches in LHC
  const unsigned int LHCOrbitRate =
      11246; // LHC orbit rate, in Hz. 299792458 / 26659
  const unsigned int LHCBCRate =
      LHCOrbitRate * LHCBunches; // LHC bunch crossing rate, in Hz

  int cruBlockSize; // size of 1 data block (RDH+payload)
  int bcStep; // interval in BC clocks between two CRU block transfers, based on
              // link input data rate

  int cfgHBperiod =
      1; // interval between 2 HeartBeat triggers, in number of LHC orbits
  double cfgGbtLinkThroughput =
      3.2; // input link data rate in Gigabits/s per second, for one link
           // (GBT=3.2 or 4.8 gbps)

  int cfgMaxBlocksPerPage; // max number of CRU blocks per page (0 => fill the
                           // page)

  double cfgEmptyHbRatio = 0.0;   // amount of empty HB frames
  int cfgPayloadSize = 64 * 1024; // maximum payload size, randomized

  class linkState {
  public:
    int HBpagecount = 0;
    int isEmpty = 0;
    int payloadBytesLeft = -1;
  };
  std::map<int, linkState> perLinkState;

  uint32_t LHCorbit = 0; // current LHC orbit
  uint32_t LHCbc = 0;    // current LHC bunch crossing

  Timer elapsedTime; // elapsed time since equipment started
  double t0 = 0;     // time of first block generated

  std::unique_ptr<AliceO2::Common::Fifo<DataBlockContainerReference>>
      readyBlocks; // pages ready to be retrieved by getNextBlock()
  std::vector<DataBlockContainerReference>
      pendingBlocks; // pages being filled (1 per link)
};

ReadoutEquipmentCruEmulator::ReadoutEquipmentCruEmulator(
    ConfigFile &cfg, std::string cfgEntryPoint)
    : ReadoutEquipment(cfg, cfgEntryPoint) {

  // declare RDH equipment
  initRdhEquipment();

  // get configuration values
  // configuration parameter: | equipment-cruemulator-* | maxBlocksPerPage | int
  // | 0 | [obsolete- not used]. Maximum number of blocks per page. |
  // configuration parameter: | equipment-cruemulator-* | cruBlockSize | int |
  // 8192 | Size of a RDH block. |
  // configuration parameter: |
  // equipment-cruemulator-* | numberOfLinks | int | 1 | Number of GBT links
  // simulated by equipment. |
  // configuration parameter: |
  // equipment-cruemulator-* | feeId | int | 0 | Front-End Electronics Id, used
  // for FEE Id field in RDH. |
  // configuration parameter: |
  // equipment-cruemulator-* | linkId | int | 0 | Id of first link. If
  // numberOfLinks>1, ids will range from linkId to linkId+numberOfLinks-1. |
  // configuration parameter: | equipment-cruemulator-* | HBperiod | int | 1 |
  // Interval between 2 HeartBeat triggers, in number of LHC orbits. |
  // configuration parameter: | equipment-cruemulator-* | EmptyHbRatio | double
  // | 0 | Fraction of empty HBframes, to simulate triggered detectors. |
  // configuration parameter: | equipment-cruemulator-* | PayloadSize | int |
  // 64k | Maximum payload size for each trigger. Actual size is randomized, and
  // then split in a number of (cruBlockSize) packets. |

  cfg.getOptionalValue<int>(cfgEntryPoint + ".maxBlocksPerPage",
                            cfgMaxBlocksPerPage, (int)0);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".cruBlockSize", cruBlockSize,
                            (int)8192);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".numberOfLinks", cfgNumberOfLinks,
                            (int)1);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".feeId", cfgFeeId, (int)0);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".linkId", cfgLinkId, (int)0);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".HBperiod", cfgHBperiod);
  cfg.getOptionalValue<double>(cfgEntryPoint + ".EmptyHbRatio",
                               cfgEmptyHbRatio);
  cfg.getOptionalValue<int>(cfgEntryPoint + ".PayloadSize", cfgPayloadSize);

  // log config summary
  theLog.log(LogInfoDevel_(3002), "Equipment %s: maxBlocksPerPage=%d cruBlockSize=%d "
             "numberOfLinks=%d feeId=%d linkId=%d HBperiod=%d "
             "EmptyHbRatio=%f PayloadSize=%d",
             name.c_str(), cfgMaxBlocksPerPage, cruBlockSize, cfgNumberOfLinks,
             cfgFeeId, cfgLinkId, cfgHBperiod, cfgEmptyHbRatio,
             cfgPayloadSize);

  // initialize array of pending blocks (to be filled with data)
  pendingBlocks.resize(cfgNumberOfLinks);

  // output queue: 1 block per link
  readyBlocks =
      std::make_unique<AliceO2::Common::Fifo<DataBlockContainerReference>>(
          cfgNumberOfLinks);
  if (readyBlocks == nullptr) {
    throw __LINE__;
  }

  // init parameters
  bcStep = (int)(LHCBCRate *
                 ((cruBlockSize - sizeof(o2::Header::RAWDataHeader)) * 1.0 /
                  (cfgGbtLinkThroughput * 1024 * 1024 * 1024 / 8)));
  theLog.log(LogInfoDevel_(3002), "Equipment %s: using block rate = %d BC", name.c_str(), bcStep);
}

ReadoutEquipmentCruEmulator::~ReadoutEquipmentCruEmulator() {}

Thread::CallbackResult ReadoutEquipmentCruEmulator::prepareBlocks() {

  // cru emulator creates a set of data pages for each link and put them in the
  // fifo to be retrieve by getNextBlock

  // check that we don't go faster than LHC...
  double t = elapsedTime.getTime();
  if (t0 == 0) {
    t0 = t;
  }
  if (LHCorbit > (uint32_t)((t - t0) * LHCOrbitRate)) {
    return Thread::CallbackResult::Idle;
  }

  // todo: check that we don't go tooooo slow !!!

  // wait enough space available in output fifo to to prepare a new set
  if (readyBlocks->getNumberOfFreeSlots() < cfgNumberOfLinks) {
    return Thread::CallbackResult::Idle;
  }

  // get a set of new blocks from memory pool (1 per link)
  for (int i = 0; i < cfgNumberOfLinks; i++) {
    if (pendingBlocks[i] != nullptr) {
      continue;
    }
    // query memory pool for a free block
    DataBlockContainerReference nextBlock = nullptr;
    try {
      nextBlock = mp->getNewDataBlockContainer();
    } catch (...) {
    }
    if (nextBlock == nullptr) {
      // no pages left, retry later
      // todo: check how long we starve pages. monitor this counter.
      return Thread::CallbackResult::Idle;
    }
    pendingBlocks[i] = nextBlock;
    // printf("equipment %s : got block ref = %p rawptr = %p data=%p
    // \n",getName().c_str(),nextBlock,nextBlock->getData(),nextBlock->getData()->data);
  }

  // at this point, we have 1 free page per link... fill it!

  o2::Header::RAWDataHeader defaultRDH; // a default RDH

  unsigned int nowOrbit = LHCorbit;
  unsigned int nowBc = LHCbc;

  for (int currentLink = 0; currentLink < cfgNumberOfLinks; currentLink++) {

    // fill the new data page for this link
    DataBlock *b = pendingBlocks[currentLink]->getData();

    // printf ("data block %p data=%p\n",b,b->data);

    int offset; // number of bytes used in page
    int nBlocksInPage = 0;

    nowOrbit = LHCorbit;
    nowBc = LHCbc;
    uint64_t nowId = getCurrentTimeframe();

    int linkId = cfgLinkId + currentLink;
    int bytesAvailableInPage =
        b->header.dataSize; // a bit less than memoryPoolPageSize;
    // printf("bytes available: %d bytes\n",bytesAvailableInPage);

    linkState &ls = perLinkState[linkId];
    // printf("link %d: %d\n",linkId,ls.payloadBytesLeft);

    for (offset = 0; offset + cruBlockSize <= bytesAvailableInPage;
         offset += cruBlockSize) {

      if ((ls.payloadBytesLeft < 0)) {
        // this is a new HB frame

        unsigned int nextBc = nowBc + bcStep;
        unsigned int nextOrbit = nowOrbit;
        if (nextBc >= LHCBunches) {
          nextOrbit += nextBc / LHCBunches;
          nextBc = nextBc % LHCBunches;
          unsigned int nextId = getTimeframeFromOrbit(nextOrbit); // timeframe ID
          if (nextId != nowId) {
            if (offset) {
              // force page change on timeframe boundary
              // printf("TF boundary : %d != %d\n",nextId,nowId);
              break;
            } else {
              // ok to change TFid when it's the first clock step
              nowId = nextId;
            }
          }
        }
        nowBc = nextBc;
        nowOrbit = nextOrbit;

        ls.HBpagecount = 0;

        // create empty HB?
        if (rand() < cfgEmptyHbRatio * RAND_MAX) {
          ls.isEmpty = 1;
          ls.payloadBytesLeft = 0;
        } else {
          // HB with random payload size
          ls.isEmpty = 0;
          ls.payloadBytesLeft = cfgPayloadSize * (rand() * 1.0 / RAND_MAX);
        }

      } else {
        // continue with current HB
        ls.HBpagecount++;
      }

      int nowHb = nowOrbit / cfgHBperiod;
      // printf("orbit=%d bc=%d HB=%d\n",nowOrbit,nowBc,nowHb);

      // rdh as defined in:
      // https://docs.google.com/document/d/1KUoLnEw5PndVcj4FKR5cjV-MBN3Bqfx_B0e6wQOIuVE/edit#heading=h.5q65he8hp62c

      o2::Header::RAWDataHeader *rdh =
          (o2::Header::RAWDataHeader *)&b->data[offset];
      // printf("rdh=%p block=%p delta=%d\n",rdh,b->data,(int)((char *)rdh-(char
      // *)b->data));

      *rdh = defaultRDH; // reset fields to defaults
      rdh->triggerOrbit = nowOrbit;
      rdh->triggerBC = nowBc;
      rdh->heartbeatOrbit = nowHb;
      rdh->feeId = cfgFeeId;
      rdh->linkId = linkId;
      rdh->offsetNextPacket = cruBlockSize;

      rdh->pagesCounter = ls.HBpagecount;
      if (ls.payloadBytesLeft > 0) {
        int bytesNow = ls.payloadBytesLeft;
        if (bytesNow + (int)sizeof(o2::Header::RAWDataHeader) > cruBlockSize) {
          bytesNow = cruBlockSize - sizeof(o2::Header::RAWDataHeader);
        }
        ls.payloadBytesLeft -= bytesNow;
        rdh->memorySize = sizeof(o2::Header::RAWDataHeader) + bytesNow;
        if (ls.payloadBytesLeft <= 0) {
          ls.payloadBytesLeft = 0;
          rdh->stopBit = 1;
          ls.payloadBytesLeft = -1;
        }
      } else {
        rdh->memorySize = sizeof(o2::Header::RAWDataHeader);
        if (!((ls.isEmpty) && (ls.HBpagecount == 0))) {
          rdh->stopBit = 1;
          ls.payloadBytesLeft = -1;
        }
      }

      // printf("block %p offset %d / %d, link %d @ %p
      // data=%p\n",b,offset,memPoolElementSize,linkId,rdh,b->data);
      // dumpRDH(rdh);
      nBlocksInPage++;
    }

    // size used (bytes) in page is last offset
    int dSize = offset;

    // printf("wrote %d bytes\n",dSize);

    // no need to fill header defaults, this is done by getNewDataBlockContainer()
    // only adjust payload size
    b->header.dataSize = dSize;
    b->header.timeframeId = nowId;
    b->header.linkId = linkId;

    readyBlocks->push(pendingBlocks[currentLink]);
    pendingBlocks[currentLink] = nullptr;
  }
  LHCorbit = nowOrbit;
  LHCbc = nowBc;

  return Thread::CallbackResult::Ok;
}

DataBlockContainerReference ReadoutEquipmentCruEmulator::getNextBlock() {

  DataBlockContainerReference nextBlock = nullptr;
  readyBlocks->pop(nextBlock);
  return nextBlock;
}

void ReadoutEquipmentCruEmulator::initCounters() {
  // init variables
  for (auto &b : pendingBlocks) {
    b = nullptr;
  }

  readyBlocks->clear();

  elapsedTime.reset();
  t0 = 0;

  LHCorbit = 0;
  LHCbc = 0;

  for (auto &ls : perLinkState) {
    ls.second.HBpagecount = 0;
    ls.second.isEmpty = 0;
    ls.second.payloadBytesLeft = -1;
  }
}

void ReadoutEquipmentCruEmulator::finalCounters() {
  // flush queue of prepared blocks
  DataBlockContainerReference nextBlock = nullptr;
  for (;;) {
    if (readyBlocks->pop(nextBlock)) {
      break;
    }
  }
}

std::unique_ptr<ReadoutEquipment>
getReadoutEquipmentCruEmulator(ConfigFile &cfg, std::string cfgEntryPoint) {
  return std::make_unique<ReadoutEquipmentCruEmulator>(cfg, cfgEntryPoint);
}
