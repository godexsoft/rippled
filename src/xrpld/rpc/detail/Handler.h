#pragma once

#include <xrpld/rpc/RPCHandler.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/Status.h>
#include <xrpld/rpc/detail/Tuning.h>

#include <xrpl/basics/Log.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ApiVersion.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/server/NetworkOPs.h>

#include <functional>
#include <set>
#include <string>

namespace json {
class Object;
}  // namespace json

namespace rpc::spec {
class SpecDumpWriter;
}  // namespace rpc::spec

namespace xrpl::RPC {

// Under what condition can we call this RPC?
enum class Condition {
    NoCondition = 0,
    NeedsNetworkConnection = 1,
    NeedsCurrentLedger = 1 << 1,
    NeedsClosedLedger = 1 << 2,
};

struct Handler
{
    template <class JsonValue>
    using Method = std::function<Status(JsonContext&, JsonValue&)>;

    // Dumps the handler's input spec for a given API version; null for handlers
    // that have no spec (old-style handlers and no-input handlers like version).
    using SpecDumpFn = void (*)(rpc::spec::SpecDumpWriter&, uint32_t apiVersion);

    char const* name;
    Method<json::Value> valueMethod;
    Role role;
    RPC::Condition condition;

    uint32_t minApiVer = kApiMinimumSupportedVersion;
    uint32_t maxApiVer = kApiMaximumValidVersion;

    SpecDumpFn specDump = nullptr;
};

/**
 * Dump the input specs of every registered RPC handler to @p os for @p apiVersion.
 *
 * @param os Ostream to dump into.
 * @param apiVersion The version of the API to dump.
 */
void
dumpAllRpcSpecs(std::ostream& os, uint32_t apiVersion);

Handler const*
getHandler(uint32_t version, bool betaEnabled, std::string const&);

/**
 * Return a json::ValueType::Object with a single entry.
 */
template <class Value>
json::Value
makeObjectValue(Value const& value, json::StaticString const& field = jss::message)
{
    json::Value result(json::ValueType::Object);
    result[field] = value;
    return result;
}

/**
 * Return names of all methods.
 */
std::set<char const*>
getHandlerNames();

template <class T>
ErrorCodeI
conditionMet(Condition conditionRequired, T& context)
{
    if (context.app.getOPs().isAmendmentBlocked() && (conditionRequired != Condition::NoCondition))
    {
        return RpcAmendmentBlocked;
    }

    if (context.app.getOPs().isUNLBlocked() && (conditionRequired != Condition::NoCondition))
    {
        return RpcExpiredValidatorList;
    }

    if ((conditionRequired != Condition::NoCondition) &&
        (context.netOps.getOperatingMode() < OperatingMode::SYNCING))
    {
        JLOG(context.j.info()) << "Insufficient network mode for RPC: "
                               << context.netOps.strOperatingMode();

        if (context.apiVersion == 1)
            return RpcNoNetwork;
        return RpcNotSynced;
    }

    if (!context.app.config().standalone() && conditionRequired != Condition::NoCondition)
    {
        if (context.ledgerMaster.getValidatedLedgerAge() > Tuning::kMaxValidatedLedgerAge)
        {
            if (context.apiVersion == 1)
                return RpcNoCurrent;
            return RpcNotSynced;
        }

        auto const cID = context.ledgerMaster.getCurrentLedgerIndex();
        auto const vID = context.ledgerMaster.getValidLedgerIndex();

        if (cID + 10 < vID)
        {
            JLOG(context.j.debug()) << "Current ledger ID(" << cID
                                    << ") is less than validated ledger ID(" << vID << ")";
            if (context.apiVersion == 1)
                return RpcNoCurrent;
            return RpcNotSynced;
        }
    }

    if ((conditionRequired != Condition::NoCondition) && !context.ledgerMaster.getClosedLedger())
    {
        if (context.apiVersion == 1)
            return RpcNoClosed;
        return RpcNotSynced;
    }

    return RpcSuccess;
}

}  // namespace xrpl::RPC
