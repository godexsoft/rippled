#include <xrpld/rpc/handlers/ledger/Ledger.h>

#include <xrpld/app/ledger/LedgerToJson.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/Status.h>
#include <xrpld/rpc/detail/RPCLedgerHelpers.h>

#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/LedgerHeader.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>
#include <xrpl/server/LoadFeeTrack.h>
#include <xrpl/shamap/SHAMap.h>

#include <grpcpp/support/status.h>
#include <org/xrpl/rpc/v1/get_ledger.pb.h>
#include <rpcspec/handlers/ledger/Spec.hpp>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace xrpl {
namespace RPC {

XrplRpcSpecView
LedgerHandler::spec([[maybe_unused]] uint32_t apiVersion)
{
    return rpc::spec::handlers::ledger::kSpec;
}

LedgerHandler::LedgerHandler(JsonContext& context) : context_(context)
{
}

LedgerHandler::Input
LedgerHandler::readInput(json::Value const& params)
{
    Input input;
    auto getBool = [&](json::StaticString const& key) -> bool {
        return params.isMember(key) && params[key].asBool();
    };
    input.full = getBool(jss::full);
    input.accounts = getBool(jss::accounts);
    input.expand = getBool(jss::expand);
    input.binary = getBool(jss::binary);
    input.ownerFunds = getBool(jss::owner_funds);
    input.queue = getBool(jss::queue);
    input.transactions = getBool(jss::transactions);
    if (params.isMember(jss::ledger_hash))
        input.ledgerHash = params[jss::ledger_hash].asString();
    if (params.isMember(jss::ledger_index) && params[jss::ledger_index].isUInt())
        input.ledgerIndex = params[jss::ledger_index].asUInt();
    input.ledgerSpecified = params.isMember(jss::ledger) || params.isMember(jss::ledger_hash) ||
        params.isMember(jss::ledger_index);
    return input;
}

LedgerHandler::Result
LedgerHandler::process(Input const& input)
{
    Output out;
    out.options = (input.full ? static_cast<int>(LedgerFill::Options::Full) : 0) |
        (input.expand ? static_cast<int>(LedgerFill::Options::Expand) : 0) |
        (input.transactions ? static_cast<int>(LedgerFill::Options::DumpTxrp) : 0) |
        (input.accounts ? static_cast<int>(LedgerFill::Options::DumpState) : 0) |
        (input.binary ? static_cast<int>(LedgerFill::Options::Binary) : 0) |
        (input.ownerFunds ? static_cast<int>(LedgerFill::Options::OwnerFunds) : 0) |
        (input.queue ? static_cast<int>(LedgerFill::Options::DumpQueue) : 0);

    if (input.ledgerSpecified)
    {
        if (auto s = lookupLedger(out.ledger, context_, out.result))
            return std::unexpected{s};
    }

    if (input.full || input.accounts)
    {
        // Until some sane way to get full ledgers has been implemented,
        // disallow retrieving all state nodes.
        if (!isUnlimited(context_.role))
            return std::unexpected{Status{RpcNoPermission}};

        if (context_.app.getFeeTrack().isLoadedLocal() && !isUnlimited(context_.role))
            return std::unexpected{Status{RpcTooBusy}};

        context_.loadType =
            input.binary ? Resource::kFeeMediumBurdenRpc : Resource::kFeeHeavyBurdenRpc;
    }

    if (input.queue)
    {
        if (!out.ledger || !out.ledger->open())
        {
            // It doesn't make sense to request the queue
            // with a non-existent or closed/validated ledger.
            return std::unexpected{Status{RpcInvalidParams}};
        }

        out.queueTxs = context_.app.getTxQ().getTxs();
    }

    return out;
}

void
LedgerHandler::writeResult(json::Value& value, Output const& out)
{
    if (out.ledger)
    {
        copyFrom(value, out.result);
        addJson(value, {*out.ledger, &context_, out.options, out.queueTxs});
    }
    else
    {
        auto& master = context_.app.getLedgerMaster();
        {
            auto& closed = value[jss::closed] = json::ValueType::Object;
            addJson(closed, {*master.getClosedLedger(), &context_, 0});
        }
        {
            auto& open = value[jss::open] = json::ValueType::Object;
            addJson(open, {*master.getCurrentLedger(), &context_, 0});
        }
    }

    // Deprecated-field warnings (e.g. `type`, `ledger`) are now produced by the
    // spec's check() phase and injected by the RPC framework; see handle().
}

}  // namespace RPC

std::pair<org::xrpl::rpc::v1::GetLedgerResponse, grpc::Status>
doLedgerGrpc(RPC::GRPCContext<org::xrpl::rpc::v1::GetLedgerRequest>& context)
{
    auto begin = std::chrono::system_clock::now();
    org::xrpl::rpc::v1::GetLedgerRequest const& request = context.params;
    org::xrpl::rpc::v1::GetLedgerResponse response;
    grpc::Status const status = grpc::Status::OK;

    std::shared_ptr<ReadView const> ledger;
    if (auto status = RPC::ledgerFromRequest(ledger, context))
    {
        grpc::Status errorStatus;
        if (status.toErrorCode() == RpcInvalidParams)
        {
            errorStatus = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, status.message());
        }
        else
        {
            errorStatus = grpc::Status(grpc::StatusCode::NOT_FOUND, status.message());
        }
        return {response, errorStatus};
    }

    Serializer s;
    addRaw(ledger->header(), s, true);

    response.set_ledger_header(s.peekData().data(), s.getLength());

    if (request.transactions())
    {
        try
        {
            for (auto& i : ledger->txs)
            {
                XRPL_ASSERT(i.first, "xrpl::doLedgerGrpc : non-null transaction");
                if (request.expand())
                {
                    auto txn = response.mutable_transactions_list()->add_transactions();
                    Serializer const sTxn = i.first->getSerializer();
                    txn->set_transaction_blob(sTxn.data(), sTxn.getLength());
                    if (i.second)
                    {
                        Serializer const sMeta = i.second->getSerializer();
                        txn->set_metadata_blob(sMeta.data(), sMeta.getLength());
                    }
                }
                else
                {
                    auto const& hash = i.first->getTransactionID();
                    response.mutable_hashes_list()->add_hashes(hash.data(), hash.size());
                }
            }
        }
        catch (std::exception const& e)
        {
            JLOG(context.j.error()) << __func__ << " - Error deserializing transaction in ledger "
                                    << ledger->header().seq
                                    << " . skipping transaction and following transactions. You "
                                       "should look into this further";
        }
    }

    if (request.get_objects())
    {
        std::shared_ptr<ReadView const> const parent =
            context.app.getLedgerMaster().getLedgerBySeq(ledger->seq() - 1);

        std::shared_ptr<Ledger const> const base = std::dynamic_pointer_cast<Ledger const>(parent);
        if (!base)
        {
            grpc::Status const errorStatus{
                grpc::StatusCode::NOT_FOUND, "parent ledger not validated"};
            return {response, errorStatus};
        }

        std::shared_ptr<Ledger const> const desired =
            std::dynamic_pointer_cast<Ledger const>(ledger);
        if (!desired)
        {
            grpc::Status const errorStatus{grpc::StatusCode::NOT_FOUND, "ledger not validated"};
            return {response, errorStatus};
        }
        SHAMap::Delta differences;

        int const maxDifferences = std::numeric_limits<int>::max();

        bool const res = base->stateMap().compare(desired->stateMap(), differences, maxDifferences);
        if (!res)
        {
            grpc::Status const errorStatus{
                grpc::StatusCode::RESOURCE_EXHAUSTED,
                "too many differences between specified ledgers"};
            return {response, errorStatus};
        }

        for (auto& [k, v] : differences)
        {
            auto obj = response.mutable_ledger_objects()->add_objects();
            auto inBase = v.first;
            auto inDesired = v.second;

            obj->set_key(k.data(), k.size());
            if (inDesired)
            {
                XRPL_ASSERT(inDesired->size() > 0, "xrpl::doLedgerGrpc : non-empty desired");
                obj->set_data(inDesired->data(), inDesired->size());
            }
            if (inBase && inDesired)
            {
                obj->set_mod_type(org::xrpl::rpc::v1::RawLedgerObject::MODIFIED);
            }
            else if (inBase && !inDesired)
            {
                obj->set_mod_type(org::xrpl::rpc::v1::RawLedgerObject::DELETED);
            }
            else
            {
                obj->set_mod_type(org::xrpl::rpc::v1::RawLedgerObject::CREATED);
            }
            auto const blob = inDesired ? inDesired->slice() : inBase->slice();
            auto const objectType = static_cast<LedgerEntryType>(blob[1] << 8 | blob[2]);

            if (request.get_object_neighbors())
            {
                if (!(inBase && inDesired))
                {
                    auto lb = desired->stateMap().lowerBound(k);
                    auto ub = desired->stateMap().upperBound(k);
                    if (lb != desired->stateMap().end())
                        obj->set_predecessor(lb->key().data(), lb->key().size());
                    if (ub != desired->stateMap().end())
                        obj->set_successor(ub->key().data(), ub->key().size());
                    if (objectType == ltDIR_NODE)
                    {
                        auto sle = std::make_shared<SLE>(SerialIter{blob}, k);
                        if (!sle->isFieldPresent(sfOwner))
                        {
                            auto bookBase = keylet::quality({ltDIR_NODE, k}, 0);
                            if (!inBase && inDesired)
                            {
                                auto firstBook = desired->stateMap().upperBound(bookBase.key);
                                if (firstBook != desired->stateMap().end() &&
                                    firstBook->key() < getQualityNext(bookBase.key) &&
                                    firstBook->key() == k)
                                {
                                    auto succ = response.add_book_successors();
                                    succ->set_book_base(bookBase.key.data(), bookBase.key.size());
                                    succ->set_first_book(
                                        firstBook->key().data(), firstBook->key().size());
                                }
                            }
                            if (inBase && !inDesired)
                            {
                                auto oldFirstBook = base->stateMap().upperBound(bookBase.key);
                                if (oldFirstBook != base->stateMap().end() &&
                                    oldFirstBook->key() < getQualityNext(bookBase.key) &&
                                    oldFirstBook->key() == k)
                                {
                                    auto succ = response.add_book_successors();
                                    succ->set_book_base(bookBase.key.data(), bookBase.key.size());
                                    auto newFirstBook =
                                        desired->stateMap().upperBound(bookBase.key);

                                    if (newFirstBook != desired->stateMap().end() &&
                                        newFirstBook->key() < getQualityNext(bookBase.key))
                                    {
                                        succ->set_first_book(
                                            newFirstBook->key().data(), newFirstBook->key().size());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        response.set_objects_included(true);
        response.set_object_neighbors_included(request.get_object_neighbors());
        response.set_skiplist_included(true);
    }

    response.set_validated(context.ledgerMaster.isValidated(*ledger));

    auto end = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() * 1.0;
    // Guard the per-item rates: an empty ledger has zero objects and/or zero
    // transactions, and dividing by zero is undefined for these doubles.
    auto const numObjects = response.ledger_objects().objects_size();
    auto const numTxns = response.transactions_list().transactions_size();
    std::string const msPerObj = numObjects > 0 ? std::to_string(duration / numObjects) : "n/a";
    std::string const msPerTxn = numTxns > 0 ? std::to_string(duration / numTxns) : "n/a";
    JLOG(context.j.warn()) << __func__ << " - Extract time = " << duration
                           << " - num objects = " << numObjects << " - num txns = " << numTxns
                           << " - ms per obj " << msPerObj << " - ms per txn " << msPerTxn;

    return {response, status};
}
}  // namespace xrpl
