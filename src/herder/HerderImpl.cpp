// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/TxSetFrame.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "lib/json/json.h"
#include "scp/LocalNode.h"
#include "main/PersistentState.h"

#include "medida/meter.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "xdrpp/marshal.h"
#include "util/basen.h"

#include <ctime>

using namespace std;

namespace stellar
{

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return make_unique<HerderImpl>(app);
}

HerderImpl::SCPMetrics::SCPMetrics(Application& app)
    : mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mNominatingValue(
          app.getMetrics().NewMeter({"scp", "value", "nominating"}, "value"))
    , mValueExternalize(
          app.getMetrics().NewMeter({"scp", "value", "externalize"}, "value"))
    , mUpdatedCandidate(
          app.getMetrics().NewMeter({"scp", "value", "candidate"}, "value"))
    , mStartBallotProtocol(
          app.getMetrics().NewMeter({"scp", "ballot", "started"}, "ballot"))
    , mAcceptedBallotPrepared(app.getMetrics().NewMeter(
          {"scp", "ballot", "accepted-prepared"}, "ballot"))
    , mConfirmedBallotPrepared(app.getMetrics().NewMeter(
          {"scp", "ballot", "confirmed-prepared"}, "ballot"))
    , mAcceptedCommit(app.getMetrics().NewMeter(
          {"scp", "ballot", "accepted-commit"}, "ballot"))
    , mBallotExpire(
          app.getMetrics().NewMeter({"scp", "ballot", "expire"}, "ballot"))

    , mQuorumHeard(
          app.getMetrics().NewMeter({"scp", "quorum", "heard"}, "quorum"))

    , mLostSync(app.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"))

    , mEnvelopeEmit(
          app.getMetrics().NewMeter({"scp", "envelope", "emit"}, "envelope"))
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))
    , mEnvelopeSign(
          app.getMetrics().NewMeter({"scp", "envelope", "sign"}, "envelope"))
    , mEnvelopeValidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "validsig"}, "envelope"))
    , mEnvelopeInvalidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "invalidsig"}, "envelope"))

    , mKnownSlotsSize(
          app.getMetrics().NewCounter({"scp", "memory", "known-slots"}))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))

    , mHerderStateCurrent(
          app.getMetrics().NewCounter({"herder", "state", "current"}))
    , mHerderStateChanges(
          app.getMetrics().NewTimer({"herder", "state", "changes"}))

    , mHerderPendingTxs0(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age0"}))
    , mHerderPendingTxs1(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age1"}))
    , mHerderPendingTxs2(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age2"}))
    , mHerderPendingTxs3(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age3"}))
{
}

HerderImpl::HerderImpl(Application& app)
    : mSCP(*this, app.getConfig().NODE_SEED, app.getConfig().NODE_IS_VALIDATOR,
           app.getConfig().QUORUM_SET)
    , mReceivedTransactions(4)
    , mPendingEnvelopes(app, *this)
    , mLastSlotSaved(0)
    , mLastStateChange(app.getClock().now())
    , mTrackingTimer(app)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app)
    , mRebroadcastTimer(app)
    , mApp(app)
    , mLedgerManager(app.getLedgerManager())
    , mSCPMetrics(app)
{
    Hash hash = sha256(xdr::xdr_to_opaque(app.getConfig().QUORUM_SET));
    mPendingEnvelopes.recvSCPQuorumSet(hash, app.getConfig().QUORUM_SET);
}

HerderImpl::~HerderImpl()
{
}

Herder::State
HerderImpl::getState() const
{
    return mTrackingSCP ? HERDER_TRACKING_STATE : HERDER_SYNCING_STATE;
}

void
HerderImpl::syncMetrics()
{
    int64_t c = mSCPMetrics.mHerderStateCurrent.count();
    int64_t n = static_cast<int64_t>(getState());
    if (c != n)
    {
        mSCPMetrics.mHerderStateCurrent.set_count(n);
    }
}

std::string
HerderImpl::getStateHuman() const
{
    static const char* stateStrings[HERDER_NUM_STATE] = {
        "HERDER_SYNCING_STATE", "HERDER_TRACKING_STATE"};
    return std::string(stateStrings[getState()]);
}

void
HerderImpl::stateChanged()
{
    mSCPMetrics.mHerderStateCurrent.set_count(static_cast<int64_t>(getState()));
    auto now = mApp.getClock().now();
    mSCPMetrics.mHerderStateChanges.Update(now - mLastStateChange);
    mLastStateChange = now;
    mApp.syncOwnMetrics();
}

void
HerderImpl::bootstrap()
{
    CLOG(INFO, "Herder") << "Force joining SCP with local state";
    assert(mSCP.isValidator());
    assert(mApp.getConfig().FORCE_SCP);

    // setup a sufficient state that we can participate in consensus
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    mTrackingSCP =
        make_unique<ConsensusData>(lcl.header.ledgerSeq, lcl.header.scpValue);
    mLedgerManager.setState(LedgerManager::LM_SYNCED_STATE);
    stateChanged();

    trackingHeartBeat();
    mLastTrigger = mApp.getClock().now() - Herder::EXP_LEDGER_TIMESPAN_SECONDS;
    ledgerClosed();
}

bool
HerderImpl::isSlotCompatibleWithCurrentState(uint64 slotIndex)
{
    bool res = false;
    if (mLedgerManager.isSynced())
    {
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
        res = (slotIndex == (lcl.header.ledgerSeq + 1));
    }

    return res;
}

SCPDriver::ValidationLevel
HerderImpl::validateValueHelper(uint64 slotIndex, StellarValue const& b)
{
    uint64 lastCloseTime;

    bool compat = isSlotCompatibleWithCurrentState(slotIndex);

    if (compat)
    {
        lastCloseTime = mLedgerManager.getLastClosedLedgerHeader()
                            .header.scpValue.closeTime;
    }
    else
    {
        if (!mTrackingSCP)
        {
            // if we're not tracking, there is not much more we can do to
            // validate
            return SCPDriver::kMaybeValidValue;
        }

        // Check slotIndex.
        if (nextConsensusLedgerIndex() > slotIndex)
        {
            // we already moved on from this slot
            // still send it through for emitting the final messages
            return SCPDriver::kMaybeValidValue;
        }
        if (nextConsensusLedgerIndex() < slotIndex)
        {
            // this is probably a bug as "tracking" means we're processing
            // messages
            // only for the right slot
            CLOG(ERROR, "Herder")
                << "HerderImpl::validateValue"
                << " i: " << slotIndex
                << " processing a future message while tracking";

            return SCPDriver::kInvalidValue;
        }
        lastCloseTime = mTrackingSCP->mConsensusValue.closeTime;
    }

    // Check closeTime (not too old)
    if (b.closeTime <= lastCloseTime)
    {
        return SCPDriver::kInvalidValue;
    }

    // Check closeTime (not too far in future)
    uint64_t timeNow = mApp.timeNow();
    if (b.closeTime > timeNow + MAX_TIME_SLIP_SECONDS.count())
    {
        return SCPDriver::kInvalidValue;
    }

    if (!compat)
    {
        // this is as far as we can go if we don't have the state
        return SCPDriver::kMaybeValidValue;
    }

    Hash txSetHash = b.txSetHash;

    // we are fully synced up

    TxSetFramePtr txSet = mPendingEnvelopes.getTxSet(txSetHash);

    SCPDriver::ValidationLevel res;

    if (!txSet)
    {
        CLOG(ERROR, "Herder") << "HerderImpl::validateValue"
                              << " i: " << slotIndex << " txSet not found?";

        res = SCPDriver::kInvalidValue;
    }
    else if (!txSet->checkValid(mApp))
    {
        CLOG(DEBUG, "Herder") << "HerderImpl::validateValue"
                              << " i: " << slotIndex << " Invalid txSet:"
                              << " " << hexAbbrev(txSet->getContentsHash());
        res = SCPDriver::kInvalidValue;
    }
    else
    {
        CLOG(DEBUG, "Herder")
            << "HerderImpl::validateValue"
            << " i: " << slotIndex
            << " txSet: " << hexAbbrev(txSet->getContentsHash()) << " OK";
        res = SCPDriver::kFullyValidatedValue;
    }
    return res;
}

bool
HerderImpl::validateUpgradeStep(uint64 slotIndex, UpgradeType const& upgrade,
                                LedgerUpgradeType& upgradeType)
{
    LedgerUpgrade lupgrade;

    try
    {
        xdr::xdr_from_opaque(upgrade, lupgrade);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return false;
    }

    bool res;
    switch (lupgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = lupgrade.newLedgerVersion();
        res = (newVersion == mApp.getConfig().LEDGER_PROTOCOL_VERSION);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        // allow fee to move within a 2x distance from the one we have in our
        // config
        res = (newFee >= mApp.getConfig().DESIRED_BASE_FEE * .5) &&
              (newFee <= mApp.getConfig().DESIRED_BASE_FEE * 2);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        // allow max to be within 30% of the config value
        uint32 newMax = lupgrade.newMaxTxSetSize();
        res = (newMax >= mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER * 7 / 10) &&
              (newMax <= mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER * 13 / 10);
    }
    break;
    default:
        res = false;
    }
    if (res)
    {
        upgradeType = lupgrade.type();
    }
    return res;
}

void
HerderImpl::signEnvelope(SCPEnvelope& envelope)
{
    mSCPMetrics.mEnvelopeSign.Mark();
    envelope.signature = mSCP.getSecretKey().sign(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
}

bool
HerderImpl::verifyEnvelope(SCPEnvelope const& envelope)
{
    bool b = PubKeyUtils::verifySig(
        envelope.statement.nodeID, envelope.signature,
        xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCP,
                           envelope.statement));
    if (b)
    {
        mSCPMetrics.mEnvelopeValidSig.Mark();
    }
    else
    {
        mSCPMetrics.mEnvelopeInvalidSig.Mark();
    }

    return b;
}

SCPDriver::ValidationLevel
HerderImpl::validateValue(uint64 slotIndex, Value const& value)
{
    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        mSCPMetrics.mValueInvalid.Mark();
        return SCPDriver::kInvalidValue;
    }

    SCPDriver::ValidationLevel res = validateValueHelper(slotIndex, b);
    if (res != SCPDriver::kInvalidValue)
    {
        LedgerUpgradeType lastUpgradeType = LEDGER_UPGRADE_VERSION;
        // check upgrades
        for (size_t i = 0; i < b.upgrades.size(); i++)
        {
            LedgerUpgradeType thisUpgradeType;
            if (!validateUpgradeStep(slotIndex, b.upgrades[i], thisUpgradeType))
            {
                CLOG(TRACE, "Herder")
                    << "HerderImpl::validateValue invalid step at index " << i;
                res = SCPDriver::kInvalidValue;
            }
            if (i != 0 && (lastUpgradeType >= thisUpgradeType))
            {
                CLOG(TRACE, "Herder") << "HerderImpl::validateValue out of "
                                         "order upgrade step at index " << i;
                res = SCPDriver::kInvalidValue;
            }

            lastUpgradeType = thisUpgradeType;
        }
    }

    if (res)
    {
        mSCPMetrics.mValueValid.Mark();
    }
    else
    {
        mSCPMetrics.mValueInvalid.Mark();
    }
    return res;
}

Value
HerderImpl::extractValidValue(uint64 slotIndex, Value const& value)
{
    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        return Value();
    }
    Value res;
    if (validateValueHelper(slotIndex, b) == SCPDriver::kFullyValidatedValue)
    {
        // remove the upgrade steps we don't like
        LedgerUpgradeType thisUpgradeType;
        for (auto it = b.upgrades.begin(); it != b.upgrades.end();)
        {

            if (!validateUpgradeStep(slotIndex, *it, thisUpgradeType))
            {
                it = b.upgrades.erase(it);
            }
            else
            {
                it++;
            }
        }

        res = xdr::xdr_to_opaque(b);
    }

    return res;
}

std::string
HerderImpl::toShortString(PublicKey const& pk) const
{
    return mApp.getConfig().toShortString(pk);
}

std::string
HerderImpl::getValueString(Value const& v) const
{
    StellarValue b;
    if (v.empty())
    {
        return "[:empty:]";
    }

    try
    {
        xdr::xdr_from_opaque(v, b);

        return stellarValueToString(b);
    }
    catch (...)
    {
        return "[:invalid:]";
    }
}

void
HerderImpl::ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mQuorumHeard.Mark();
}

void
HerderImpl::updateSCPCounters()
{
    mSCPMetrics.mKnownSlotsSize.set_count(mSCP.getKnownSlotsCount());
    mSCPMetrics.mCumulativeStatements.set_count(
        mSCP.getCumulativeStatemtCount());
}

static uint64_t
countTxs(HerderImpl::AccountTxMap const& acc)
{
    uint64_t sz = 0;
    for (auto const& a : acc)
    {
        sz += a.second->mTransactions.size();
    }
    return sz;
}

static std::shared_ptr<HerderImpl::TxMap>
findOrAdd(HerderImpl::AccountTxMap& acc, AccountID const& aid)
{
    std::shared_ptr<HerderImpl::TxMap> txmap = nullptr;
    auto i = acc.find(aid);
    if (i == acc.end())
    {
        txmap = std::make_shared<HerderImpl::TxMap>();
        acc.insert(std::make_pair(aid, txmap));
    }
    else
    {
        txmap = i->second;
    }
    return txmap;
}

void
HerderImpl::logQuorumInformation(uint64 index)
{
    std::string res;
    Json::Value v;
    dumpQuorumInfo(v, mSCP.getLocalNodeID(), true, index);
    auto slots = v.get("slots", "");
    if (!slots.empty())
    {
        std::string indexs = std::to_string(static_cast<uint32>(index));
        auto i = slots.get(indexs, "");
        if (!i.empty())
        {
            Json::FastWriter fw;
            CLOG(INFO, "Herder") << "Quorum information for " << index << " : "
                                 << fw.write(i);
        }
    }
}

void
HerderImpl::valueExternalized(uint64 slotIndex, Value const& value)
{
    updateSCPCounters();
    mSCPMetrics.mValueExternalize.Mark();
    mSCPTimers.erase(slotIndex); // cancels all timers for this slot
    if (slotIndex <= getCurrentLedgerSeq())
    {
        // externalize may trigger on older slots:
        //  * when the current instance starts up
        //  * when getting back in sync (a gap potentially opened)
        // in both cases it's safe to just ignore those as we're already
        // tracking a more recent state
        CLOG(DEBUG, "Herder") << "Ignoring old ledger externalize "
                              << slotIndex;
        return;
    }

    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        // This may not be possible as all messages are validated and should
        // therefore contain a valid StellarValue.
        CLOG(ERROR, "Herder") << "HerderImpl::valueExternalized"
                              << " Externalized StellarValue malformed";
        // no point in continuing as 'b' contains garbage at this point
        abort();
    }

    Hash const& txSetHash = b.txSetHash;

    CLOG(DEBUG, "Herder") << "HerderImpl::valueExternalized"
                          << " txSet: " << hexAbbrev(txSetHash);

    // log information from older ledger to increase the chances that
    // all messages made it
    if (slotIndex > 2)
    {
        logQuorumInformation(slotIndex - 2);
    }

    // current value is not valid anymore
    mCurrentValue.clear();

    if (!mTrackingSCP)
    {
        stateChanged();
    }

    mTrackingSCP = make_unique<ConsensusData>(slotIndex, b);
    trackingHeartBeat();

    TxSetFramePtr externalizedSet = mPendingEnvelopes.getTxSet(txSetHash);

    // trigger will be recreated when the ledger is closed
    // we do not want it to trigger while downloading the current set
    // and there is no point in taking a position after the round is over
    mTriggerTimer.cancel();

    // tell the LedgerManager that this value got externalized
    // LedgerManager will perform the proper action based on its internal
    // state: apply, trigger catchup, etc
    LedgerCloseData ledgerData(lastConsensusLedgerIndex(), externalizedSet, b);
    mLedgerManager.externalizeValue(ledgerData);

    // perform cleanups

    // remove all these tx from mReceivedTransactions
    removeReceivedTxs(externalizedSet->mTransactions);

    // rebroadcast those left in set 1, sorted in an apply-order.
    {
        Hash h;
        TxSetFrame broadcast(h);
        assert(mReceivedTransactions.size() >= 2);
        for (auto const& pair : mReceivedTransactions[1])
        {
            for (auto const& tx : pair.second->mTransactions)
            {
                broadcast.add(tx.second);
            }
        }
        for (auto tx : broadcast.sortForApply())
        {
            auto msg = tx->toStellarMessage();
            mApp.getOverlayManager().broadcastMessage(msg);
        }
    }

    // Evict slots that are outside of our ledger validity bracket
    if (slotIndex > MAX_SLOTS_TO_REMEMBER)
    {
        mSCP.purgeSlots(slotIndex - MAX_SLOTS_TO_REMEMBER);
    }

    assert(mReceivedTransactions.size() >= 4);

    // Move all the remaining to the next highest level don't move the
    // largest array.
    for (size_t n = mReceivedTransactions.size() - 1; n > 0; n--)
    {
        auto& curr = mReceivedTransactions[n];
        auto& prev = mReceivedTransactions[n - 1];
        for (auto const& pair : prev)
        {
            auto const& acc = pair.first;
            auto const& srcmap = pair.second;
            auto dstmap = findOrAdd(curr, acc);
            for (auto tx : srcmap->mTransactions)
            {
                dstmap->addTx(tx.second);
            }
        }
        prev.clear();
    }

    mSCPMetrics.mHerderPendingTxs0.set_count(
        countTxs(mReceivedTransactions[0]));
    mSCPMetrics.mHerderPendingTxs1.set_count(
        countTxs(mReceivedTransactions[1]));
    mSCPMetrics.mHerderPendingTxs2.set_count(
        countTxs(mReceivedTransactions[2]));
    mSCPMetrics.mHerderPendingTxs3.set_count(
        countTxs(mReceivedTransactions[3]));

    ledgerClosed();
}

void
HerderImpl::nominatingValue(uint64 slotIndex, Value const& value)
{
    CLOG(DEBUG, "Herder") << "nominatingValue i:" << slotIndex
                          << " v: " << getValueString(value);

    if (!value.empty())
    {
        mSCPMetrics.mNominatingValue.Mark();
    }
}

Value
HerderImpl::combineCandidates(uint64 slotIndex,
                              std::set<Value> const& candidates)
{
    Hash h;
    xdr::xvector<UpgradeType, 4> emptyV;

    StellarValue comp(h, 0, emptyUpgradeSteps, 0);

    std::map<LedgerUpgradeType, LedgerUpgrade> upgrades;

    std::set<TransactionFramePtr> aggSet;

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    Hash candidatesHash;

    std::vector<StellarValue> candidateValues;

    for (auto const& c : candidates)
    {
        candidateValues.emplace_back();
        StellarValue& sv = candidateValues.back();

        xdr::xdr_from_opaque(c, sv);
        candidatesHash ^= sha256(c);

        // max closeTime
        if (comp.closeTime < sv.closeTime)
        {
            comp.closeTime = sv.closeTime;
        }
        for (auto const& upgrade : sv.upgrades)
        {
            LedgerUpgrade lupgrade;
            xdr::xdr_from_opaque(upgrade, lupgrade);
            auto it = upgrades.find(lupgrade.type());
            if (it == upgrades.end())
            {
                upgrades.emplace(std::make_pair(lupgrade.type(), lupgrade));
            }
            else
            {
                LedgerUpgrade& clUpgrade = it->second;
                switch (lupgrade.type())
                {
                case LEDGER_UPGRADE_VERSION:
                    // pick the highest version
                    if (clUpgrade.newLedgerVersion() <
                        lupgrade.newLedgerVersion())
                    {
                        clUpgrade.newLedgerVersion() =
                            lupgrade.newLedgerVersion();
                    }
                    break;
                case LEDGER_UPGRADE_BASE_FEE:
                    // take the max fee
                    if (clUpgrade.newBaseFee() < lupgrade.newBaseFee())
                    {
                        clUpgrade.newBaseFee() = lupgrade.newBaseFee();
                    }
                    break;
                case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
                    // take the max tx set size
                    if (clUpgrade.newMaxTxSetSize() <
                        lupgrade.newMaxTxSetSize())
                    {
                        clUpgrade.newMaxTxSetSize() =
                            lupgrade.newMaxTxSetSize();
                    }
                    break;
                default:
                    // should never get there with values that are not valid
                    throw std::runtime_error("invalid upgrade step");
                }
            }
        }
    }

    // take the txSet with the highest number of transactions,
    // highest xored hash that we have
    Hash highest;
    TxSetFramePtr bestTxSet;
    for (auto const& sv : candidateValues)
    {
        TxSetFramePtr cTxSet = getTxSet(sv.txSetHash);

        if (cTxSet && cTxSet->previousLedgerHash() == lcl.hash)
        {
            if (!bestTxSet || (cTxSet->mTransactions.size() >
                               bestTxSet->mTransactions.size()) ||
                ((cTxSet->mTransactions.size() ==
                  bestTxSet->mTransactions.size()) &&
                 lessThanXored(highest, sv.txSetHash, candidatesHash)))
            {
                bestTxSet = cTxSet;
                highest = sv.txSetHash;
            }
        }
    }

    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade.second));
        comp.upgrades.emplace_back(v.begin(), v.end());
    }

    std::vector<TransactionFramePtr> removed;

    // just to be sure
    bestTxSet->trimInvalid(mApp, removed);
    comp.txSetHash = bestTxSet->getContentsHash();

    if (removed.size() != 0)
    {
        CLOG(WARNING, "Herder") << "Candidate set had " << removed.size()
                                << " invalid transactions";

        // post to avoid triggering SCP handling code recursively
        mApp.getClock().getIOService().post(
            [this, bestTxSet]()
            {
                mPendingEnvelopes.recvTxSet(bestTxSet->getContentsHash(),
                                            bestTxSet);
            });
    }

    return xdr::xdr_to_opaque(comp);
}

void
HerderImpl::setupTimer(uint64 slotIndex, int timerID,
                       std::chrono::milliseconds timeout,
                       std::function<void()> cb)
{
    // don't setup timers for old slots
    if (mTrackingSCP && slotIndex < mTrackingSCP->mConsensusIndex)
    {
        mSCPTimers.erase(slotIndex);
        return;
    }

    auto& slotTimers = mSCPTimers[slotIndex];

    auto it = slotTimers.find(timerID);
    if (it == slotTimers.end())
    {
        it = slotTimers.emplace(timerID, make_unique<VirtualTimer>(mApp)).first;
    }
    auto& timer = *it->second;
    timer.cancel();
    timer.expires_from_now(timeout);
    timer.async_wait(cb, &VirtualTimer::onFailureNoop);
}

void
HerderImpl::rebroadcast()
{
    for (auto const& e :
         mSCP.getLatestMessagesSend(mLedgerManager.getLedgerNum()))
    {
        broadcast(e);
    }
    startRebroadcastTimer();
}

void
HerderImpl::broadcast(SCPEnvelope const& e)
{
    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        StellarMessage m;
        m.type(SCP_MESSAGE);
        m.envelope() = e;

        CLOG(DEBUG, "Herder") << "broadcast "
                              << " s:" << e.statement.pledges.type()
                              << " i:" << e.statement.slotIndex;

        mSCPMetrics.mEnvelopeEmit.Mark();
        mApp.getOverlayManager().broadcastMessage(m, true);
    }
}

void
HerderImpl::startRebroadcastTimer()
{
    mRebroadcastTimer.expires_from_now(std::chrono::seconds(2));

    mRebroadcastTimer.async_wait(std::bind(&HerderImpl::rebroadcast, this),
                                 &VirtualTimer::onFailureNoop);
}

void
HerderImpl::emitEnvelope(SCPEnvelope const& envelope)
{
    uint64 slotIndex = envelope.statement.slotIndex;

    CLOG(DEBUG, "Herder") << "emitEnvelope"
                          << " s:" << envelope.statement.pledges.type()
                          << " i:" << slotIndex
                          << " a:" << mApp.getStateHuman();

    persistSCPState(slotIndex);

    broadcast(envelope);

    // this resets the re-broadcast timer
    startRebroadcastTimer();
}

bool
HerderImpl::recvTransactions(TxSetFramePtr txSet)
{
    soci::transaction sqltx(mApp.getDatabase().getSession());
    mApp.getDatabase().setCurrentTransactionReadOnly();

    bool allGood = true;
    for (auto tx : txSet->sortForApply())
    {
        if (recvTransaction(tx) != TX_STATUS_PENDING)
        {
            allGood = false;
        }
    }
    return allGood;
}

void
HerderImpl::TxMap::addTx(TransactionFramePtr tx)
{
    auto const& h = tx->getFullHash();
    if (mTransactions.find(h) != mTransactions.end())
    {
        return;
    }
    mTransactions.insert(std::make_pair(h, tx));
    mMaxSeq = std::max(tx->getSeqNum(), mMaxSeq);
    mTotalFees += tx->getFee();
}

void
HerderImpl::TxMap::recalculate()
{
    mMaxSeq = 0;
    mTotalFees = 0;
    for (auto const& pair : mTransactions)
    {
        mMaxSeq = std::max(pair.second->getSeqNum(), mMaxSeq);
        mTotalFees += pair.second->getFee();
    }
}

Herder::TransactionSubmitStatus
HerderImpl::recvTransaction(TransactionFramePtr tx)
{
    soci::transaction sqltx(mApp.getDatabase().getSession());
    mApp.getDatabase().setCurrentTransactionReadOnly();

    auto const& acc = tx->getSourceID();
    auto const& txID = tx->getFullHash();

    // determine if we have seen this tx before and if not if it has the right
    // seq num
    int64_t totFee = tx->getFee();
    SequenceNumber highSeq = 0;

    for (auto& map : mReceivedTransactions)
    {
        auto i = map.find(acc);
        if (i != map.end())
        {
            auto& txmap = i->second;
            auto j = txmap->mTransactions.find(txID);
            if (j != txmap->mTransactions.end())
            {
                return TX_STATUS_DUPLICATE;
            }
            totFee += txmap->mTotalFees;
            highSeq = std::max(highSeq, txmap->mMaxSeq);
        }
    }

    if (!tx->checkValid(mApp, highSeq))
    {
        return TX_STATUS_ERROR;
    }

    if (tx->getSourceAccount().getBalanceAboveReserve(mLedgerManager) < totFee)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return TX_STATUS_ERROR;
    }

    CLOG(TRACE, "Herder") << "recv transaction " << hexAbbrev(txID) << " for "
                          << PubKeyUtils::toShortString(acc);

    auto txmap = findOrAdd(mReceivedTransactions[0], acc);
    txmap->addTx(tx);

    return TX_STATUS_PENDING;
}

void
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    CLOG(DEBUG, "Herder") << "recvSCPEnvelope"
                          << " from: "
                          << mApp.getConfig().toShortString(
                                 envelope.statement.nodeID)
                          << " s:" << envelope.statement.pledges.type()
                          << " i:" << envelope.statement.slotIndex
                          << " a:" << mApp.getStateHuman();

    if (envelope.statement.nodeID == mSCP.getLocalNode()->getNodeID())
    {
        CLOG(DEBUG, "Herder") << "recvSCPEnvelope: skipping own message";
        return;
    }

    mSCPMetrics.mEnvelopeReceive.Mark();

    if (mTrackingSCP)
    {
        // when tracking, we can filter messages based on the information we got
        // from consensus
        uint32_t minLedgerSeq = nextConsensusLedgerIndex();
        if (minLedgerSeq > MAX_SLOTS_TO_REMEMBER)
        {
            minLedgerSeq -= MAX_SLOTS_TO_REMEMBER;
        }
        uint32_t maxLedgerSeq =
            nextConsensusLedgerIndex() + LEDGER_VALIDITY_BRACKET;

        // If we are fully synced and the envelopes are out of our validity
        // brackets, we just ignore them.
        if (envelope.statement.slotIndex > maxLedgerSeq ||
            envelope.statement.slotIndex < minLedgerSeq)
        {
            CLOG(DEBUG, "Herder") << "Ignoring SCPEnvelope outside of range: "
                                  << envelope.statement.slotIndex << "( "
                                  << minLedgerSeq << "," << maxLedgerSeq << ")";
            return;
        }
    }

    mPendingEnvelopes.recvSCPEnvelope(envelope);
}

void
HerderImpl::sendSCPStateToPeer(uint32 ledgerSeq, PeerPtr peer)
{
    uint32 minSeq, maxSeq;

    if (ledgerSeq == 0)
    {
        const uint32 nbLedgers = 3;
        maxSeq = getCurrentLedgerSeq();
        if (maxSeq >= 2 + nbLedgers)
        {
            minSeq = maxSeq - nbLedgers;
        }
        else
        {
            minSeq = 2;
        }
    }
    else
    {
        minSeq = maxSeq = ledgerSeq;
    }

    for (uint32 seq = minSeq; seq <= maxSeq; seq++)
    {
        auto const& envelopes = mSCP.getCurrentState(seq);

        CLOG(DEBUG, "Herder") << "Send state " << envelopes.size()
                              << " for ledger " << seq;

        for (auto const& e : envelopes)
        {
            StellarMessage m;
            m.type(SCP_MESSAGE);
            m.envelope() = e;

            mSCPMetrics.mEnvelopeEmit.Mark();
            peer->sendMessage(m);
        }
    }
}

void
HerderImpl::processSCPQueue()
{
    if (mTrackingSCP)
    {
        // drop obsolete slots
        if (nextConsensusLedgerIndex() > MAX_SLOTS_TO_REMEMBER)
        {
            mPendingEnvelopes.eraseBelow(nextConsensusLedgerIndex() -
                                         MAX_SLOTS_TO_REMEMBER);
        }

        processSCPQueueUpToIndex(nextConsensusLedgerIndex());
    }
    else
    {
        // we don't know which ledger we're in
        // try to consume the messages from the queue
        // starting from the smallest slot
        for (auto& slot : mPendingEnvelopes.readySlots())
        {
            processSCPQueueUpToIndex(slot);
            if (mTrackingSCP)
            {
                // one of the slots externalized
                // we go back to regular flow
                break;
            }
        }
    }
}

void
HerderImpl::processSCPQueueUpToIndex(uint64 slotIndex)
{
    while (true)
    {
        SCPEnvelope env;
        if (mPendingEnvelopes.pop(slotIndex, env))
        {
            mSCP.receiveEnvelope(env);
        }
        else
        {
            return;
        }
    }
}

void
HerderImpl::ledgerClosed()
{
    mTriggerTimer.cancel();

    updateSCPCounters();
    CLOG(TRACE, "Herder") << "HerderImpl::ledgerClosed";

    mPendingEnvelopes.slotClosed(lastConsensusLedgerIndex());

    mApp.getOverlayManager().ledgerClosed(lastConsensusLedgerIndex());

    uint64_t nextIndex = nextConsensusLedgerIndex();

    // process any statements up to this slot (this may trigger externalize)
    processSCPQueueUpToIndex(nextIndex);

    // if externalize got called for a future slot, we don't
    // need to trigger (the now obsolete) next round
    if (nextIndex != nextConsensusLedgerIndex())
    {
        return;
    }

    // If we are not a validating node and just watching SCP we don't call
    // triggerNextLedger. Likewise if we are not in synced state.
    if (!mSCP.isValidator())
    {
        CLOG(DEBUG, "Herder")
            << "Non-validating node, not triggering ledger-close.";
        return;
    }

    if (!mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder")
            << "Not presently synced, not triggering ledger-close.";
        return;
    }

    auto seconds = Herder::EXP_LEDGER_TIMESPAN_SECONDS;
    if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        seconds = std::chrono::seconds(1);
    }
    if (mApp.getConfig().ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING)
    {
        seconds = std::chrono::seconds(
            mApp.getConfig().ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING);
    }

    auto now = mApp.getClock().now();
    if ((now - mLastTrigger) < seconds)
    {
        auto timeout = seconds - (now - mLastTrigger);
        mTriggerTimer.expires_from_now(timeout);
    }
    else
    {
        mTriggerTimer.expires_from_now(std::chrono::nanoseconds(0));
    }

    if (!mApp.getConfig().MANUAL_CLOSE)
        mTriggerTimer.async_wait(std::bind(&HerderImpl::triggerNextLedger, this,
                                           static_cast<uint32_t>(nextIndex)),
                                 &VirtualTimer::onFailureNoop);
}

void
HerderImpl::removeReceivedTxs(std::vector<TransactionFramePtr> const& dropTxs)
{
    for (auto& m : mReceivedTransactions)
    {
        if (m.empty())
        {
            continue;
        }

        std::set<std::shared_ptr<TxMap>> toRecalculate;

        for (auto const& tx : dropTxs)
        {
            auto const& acc = tx->getSourceID();
            auto const& txID = tx->getFullHash();
            auto i = m.find(acc);
            if (i != m.end())
            {
                auto& txs = i->second->mTransactions;
                auto j = txs.find(txID);
                if (j != txs.end())
                {
                    txs.erase(j);
                    if (txs.empty())
                    {
                        m.erase(i);
                    }
                    else
                    {
                        toRecalculate.insert(i->second);
                    }
                }
            }
        }

        for (auto txm : toRecalculate)
        {
            txm->recalculate();
        }
    }
}

void
HerderImpl::recvSCPQuorumSet(Hash hash, const SCPQuorumSet& qset)
{
    mPendingEnvelopes.recvSCPQuorumSet(hash, qset);
}

void
HerderImpl::recvTxSet(Hash hash, const TxSetFrame& t)
{
    TxSetFramePtr txset(new TxSetFrame(t));
    mPendingEnvelopes.recvTxSet(hash, txset);
}

void
HerderImpl::peerDoesntHave(MessageType type, uint256 const& itemID,
                           PeerPtr peer)
{
    mPendingEnvelopes.peerDoesntHave(type, itemID, peer);
}

TxSetFramePtr
HerderImpl::getTxSet(Hash hash)
{
    return mPendingEnvelopes.getTxSet(hash);
}

SCPQuorumSetPtr
HerderImpl::getQSet(const Hash& qSetHash)
{
    return mPendingEnvelopes.getQSet(qSetHash);
}

uint32_t
HerderImpl::getCurrentLedgerSeq() const
{
    uint32_t res = mLedgerManager.getLastClosedLedgerNum();

    if (mTrackingSCP && res < mTrackingSCP->mConsensusIndex)
    {
        res = static_cast<uint32_t>(mTrackingSCP->mConsensusIndex);
    }
    if (mLastTrackingSCP && res < mLastTrackingSCP->mConsensusIndex)
    {
        res = static_cast<uint32_t>(mLastTrackingSCP->mConsensusIndex);
    }
    return res;
}

SequenceNumber
HerderImpl::getMaxSeqInPendingTxs(AccountID const& acc)
{
    SequenceNumber highSeq = 0;
    for (auto const& m : mReceivedTransactions)
    {
        auto i = m.find(acc);
        if (i == m.end())
        {
            continue;
        }
        highSeq = std::max(i->second->mMaxSeq, highSeq);
    }
    return highSeq;
}

// called to take a position during the next round
// uses the state in LedgerManager to derive a starting position
void
HerderImpl::triggerNextLedger(uint32_t ledgerSeqToTrigger)
{
    if (!mTrackingSCP || !mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder") << "triggerNextLedger: skipping (out of sync) : "
                              << mApp.getStateHuman();
        return;
    }
    updateSCPCounters();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    TxSetFramePtr proposedSet = std::make_shared<TxSetFrame>(lcl.hash);

    for (auto const& m : mReceivedTransactions)
    {
        for (auto const& pair : m)
        {
            for (auto const& tx : pair.second->mTransactions)
            {
                proposedSet->add(tx.second);
            }
        }
    }

    std::vector<TransactionFramePtr> removed;
    proposedSet->trimInvalid(mApp, removed);
    removeReceivedTxs(removed);

    proposedSet->surgePricingFilter(mLedgerManager);

    if (!proposedSet->checkValid(mApp))
    {
        throw std::runtime_error("wanting to emit an invalid txSet");
    }

    auto txSetHash = proposedSet->getContentsHash();

    // Inform the item fetcher so queries from other peers about his txSet
    // can be answered. Note this can trigger SCP callbacks, externalize, etc
    // if we happen to build a txset that we were trying to download.
    mPendingEnvelopes.recvTxSet(txSetHash, proposedSet);

    // use the slot index from ledger manager here as our vote is based off
    // the last closed ledger stored in ledger manager
    uint32_t slotIndex = lcl.header.ledgerSeq + 1;

    // no point in sending out a prepare:
    // externalize was triggered on a more recent ledger
    if (ledgerSeqToTrigger != slotIndex)
    {
        return;
    }

    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime = VirtualClock::to_time_t(mLastTrigger);
    if (nextCloseTime <= lcl.header.scpValue.closeTime)
    {
        nextCloseTime = lcl.header.scpValue.closeTime + 1;
    }

    StellarValue newProposedValue(txSetHash, nextCloseTime, emptyUpgradeSteps,
                                  0);

    std::vector<LedgerUpgrade> upgrades;

    // see if we need to include some upgrades
    if (lcl.header.ledgerVersion != mApp.getConfig().LEDGER_PROTOCOL_VERSION)
    {
        upgrades.emplace_back(LEDGER_UPGRADE_VERSION);
        upgrades.back().newLedgerVersion() =
            mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    }
    if (lcl.header.baseFee != mApp.getConfig().DESIRED_BASE_FEE)
    {
        upgrades.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        upgrades.back().newBaseFee() = mApp.getConfig().DESIRED_BASE_FEE;
    }
    if (lcl.header.maxTxSetSize != mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER)
    {
        upgrades.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        upgrades.back().newMaxTxSetSize() =
            mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER;
    }

    UpgradeType ut; // only used for max size check
    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade));
        if (v.size() >= ut.max_size())
        {
            CLOG(ERROR, "Herder") << "HerderImpl::triggerNextLedger"
                                  << " exceeded size for upgrade step (got "
                                  << v.size() << " ) for upgrade type "
                                  << std::to_string(upgrade.type());
        }
        else
        {
            newProposedValue.upgrades.emplace_back(v.begin(), v.end());
        }
    }

    mCurrentValue = xdr::xdr_to_opaque(newProposedValue);

    uint256 valueHash = sha256(xdr::xdr_to_opaque(mCurrentValue));
    CLOG(DEBUG, "Herder") << "HerderImpl::triggerNextLedger"
                          << " txSet.size: "
                          << proposedSet->mTransactions.size()
                          << " previousLedgerHash: "
                          << hexAbbrev(proposedSet->previousLedgerHash())
                          << " value: " << hexAbbrev(valueHash)
                          << " slot: " << slotIndex;

    Value prevValue = xdr::xdr_to_opaque(lcl.header.scpValue);

    mSCP.nominate(slotIndex, mCurrentValue, prevValue);
}

bool
HerderImpl::isQuorumSetSane(NodeID const& nodeID, SCPQuorumSet const& qSet)
{
    return mSCP.getLocalNode()->isQuorumSetSane(nodeID, qSet);
}

// Extra SCP methods overridden solely to increment metrics.
void
HerderImpl::updatedCandidateValue(uint64 slotIndex, Value const& value)
{
    mSCPMetrics.mUpdatedCandidate.Mark();
}

void
HerderImpl::startedBallotProtocol(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mStartBallotProtocol.Mark();
}
void
HerderImpl::acceptedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedBallotPrepared.Mark();
}

void
HerderImpl::confirmedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mConfirmedBallotPrepared.Mark();
}

void
HerderImpl::acceptedCommit(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedCommit.Mark();
}

void
HerderImpl::dumpInfo(Json::Value& ret)
{
    ret["you"] = mApp.getConfig().toStrKey(mSCP.getSecretKey().getPublicKey());

    mSCP.dumpInfo(ret);

    mPendingEnvelopes.dumpInfo(ret);
}

void
HerderImpl::dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                           uint64 index)
{
    ret["node"] = mApp.getConfig().toStrKey(id);

    mSCP.dumpQuorumInfo(ret["slots"], id, summary, index);
}

void
HerderImpl::persistSCPState(uint64 slot)
{
    if (slot < mLastSlotSaved)
    {
        return;
    }

    mLastSlotSaved = slot;

    // saves SCP messages and related data (transaction sets, quorum sets)
    xdr::xvector<SCPEnvelope> latestEnvs;
    std::map<Hash, TxSetFramePtr> txSets;
    std::map<Hash, SCPQuorumSetPtr> quorumSets;

    for (auto const& e : mSCP.getLatestMessagesSend(slot))
    {
        latestEnvs.emplace_back(e);

        // saves transaction sets referred by the statement
        std::vector<Value> vals = Slot::getStatementValues(e.statement);
        for (auto const& v : vals)
        {
            StellarValue wb;
            xdr::xdr_from_opaque(v, wb);
            TxSetFramePtr txSet = mPendingEnvelopes.getTxSet(wb.txSetHash);
            txSets.insert(std::make_pair(wb.txSetHash, txSet));
        }
        Hash qsHash = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        SCPQuorumSetPtr qSet = mPendingEnvelopes.getQSet(qsHash);
        quorumSets.insert(std::make_pair(qsHash, qSet));
    }

    xdr::xvector<TransactionSet> latestTxSets;
    for (auto it : txSets)
    {
        latestTxSets.emplace_back();
        it.second->toXDR(latestTxSets.back());
    }

    xdr::xvector<SCPQuorumSet> latestQSets;
    for (auto it : quorumSets)
    {
        latestQSets.emplace_back(*it.second);
    }

    auto latestSCPData =
        xdr::xdr_to_opaque(latestEnvs, latestTxSets, latestQSets);
    std::string scpState;
    scpState = bn::encode_b64(latestSCPData);

    mApp.getPersistentState().setState(PersistentState::kLastSCPData, scpState);
}

void
HerderImpl::restoreSCPState()
{
    auto latest64 =
        mApp.getPersistentState().getState(PersistentState::kLastSCPData);

    if (latest64.empty())
    {
        return;
    }

    std::vector<uint8_t> buffer;
    bn::decode_b64(latest64, buffer);

    xdr::xvector<SCPEnvelope> latestEnvs;
    xdr::xvector<TransactionSet> latestTxSets;
    xdr::xvector<SCPQuorumSet> latestQSets;

    // no exception guard here: we want to crash if we don't recognize old
    // messages
    // only way out of this situation is probably to reset the node and catchup
    // to
    // the network's state (it's unsafe to participate with bad SCP messages)
    xdr::xdr_from_opaque(buffer, latestEnvs, latestTxSets, latestQSets);

    for (auto const& txset : latestTxSets)
    {
        TxSetFramePtr cur = make_shared<TxSetFrame>(mApp.getNetworkID(), txset);
        Hash h = cur->getContentsHash();
        mPendingEnvelopes.recvTxSet(h, cur);
    }
    for (auto const& qset : latestQSets)
    {
        Hash hash = sha256(xdr::xdr_to_opaque(qset));
        mPendingEnvelopes.recvSCPQuorumSet(hash, qset);
    }
    for (auto const& e : latestEnvs)
    {
        mSCP.setStateFromEnvelope(e.statement.slotIndex, e);
    }

    if (latestEnvs.size() != 0)
    {
        mLastSlotSaved = latestEnvs.back().statement.slotIndex;
        startRebroadcastTimer();
    }
}

void
HerderImpl::trackingHeartBeat()
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    assert(mTrackingSCP);
    mTrackingTimer.expires_from_now(
        std::chrono::seconds(CONSENSUS_STUCK_TIMEOUT_SECONDS));
    mTrackingTimer.async_wait(std::bind(&HerderImpl::herderOutOfSync, this),
                              &VirtualTimer::onFailureNoop);
}

void
HerderImpl::herderOutOfSync()
{
    CLOG(INFO, "Herder") << "Lost track of consensus";
    mSCPMetrics.mLostSync.Mark();
    stateChanged();

    // transfer ownership to mLastTrackingSCP
    mLastTrackingSCP.reset(mTrackingSCP.release());

    processSCPQueue();
}
}
