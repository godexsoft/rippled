//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/Tuning.h>
#include <xrpld/rpc/openapi/AccountChannels.h>

#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>

#include <model/AccountChannelsErrorResponseCodes.hpp>
#include <model/AccountChannelsResponse.hpp>

#include <variant>

using namespace openapi_rippled::model;

namespace ripple {

void
AccountChannelsHandlerImpl::addChannel(
    std::vector<Channel>& channels,
    ripple::SLE const& channelSle)
{
    Channel channel;
    channel.setChannelId(ripple::to_string(channelSle.key()));
    channel.setAccount(
        ripple::to_string(channelSle.getAccountID(ripple::sfAccount)));
    channel.setDestinationAccount(
        ripple::to_string(channelSle.getAccountID(ripple::sfDestination)));
    channel.setAmount(channelSle[ripple::sfAmount].getText());
    channel.setBalance(channelSle[ripple::sfBalance].getText());
    channel.setSettleDelay(channelSle[ripple::sfSettleDelay]);

    if (publicKeyType(channelSle[ripple::sfPublicKey]))
    {
        ripple::PublicKey const pk(channelSle[ripple::sfPublicKey]);
        channel.setPublicKey(toBase58(ripple::TokenType::AccountPublic, pk));
        channel.setPublicKeyHex(strHex(pk));
    }

    if (auto const& v = channelSle[~ripple::sfExpiration])
        channel.setExpiration(v);

    if (auto const& v = channelSle[~ripple::sfCancelAfter])
        channel.setCancelAfter(v);

    if (auto const& v = channelSle[~ripple::sfSourceTag])
        channel.setSourceTag(v);

    if (auto const& v = channelSle[~ripple::sfDestinationTag])
        channel.setDestinationTag(v);

    channels.push_back(channel);
}

Expected<
    openapi_rippled::model::AccountChannelsSuccessResponse,
    AccountChannelsHandlerImpl::ErrorCodes>
AccountChannelsHandlerImpl::process(
    openapi_rippled::model::AccountChannelsRequestBase const& req,
    RPC::JsonContext& context)
{
    using namespace openapi_rippled::model;  // generated name of namespace
                                             // can be adjusted in openapi

    std::shared_ptr<ReadView const> ledger;

    // copy some request stuff into context.params so that we can use
    // lookupLedger to some extent
    if (req.getLedgerIndex().has_value())
        std::visit(
            [&](auto index) { context.params["ledger_index"] = index; },
            req.getLedgerIndex().value());
    if (req.getLedgerHash().has_value())
        context.params["ledger_hash"] = req.getLedgerHash().value();

    auto result = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return Unexpected(AccountChannelsErrorResponseCodes::LGRNOTFOUND);

    auto id = parseBase58<AccountID>(req.getAccount());
    if (!id)
        return Unexpected(
            AccountChannelsErrorResponseCodes::ACTNOTFOUND);  // todo: malformed

    AccountID const accountID{std::move(id.value())};
    if (!ledger->exists(keylet::account(accountID)))
        return Unexpected(AccountChannelsErrorResponseCodes::ACTNOTFOUND);

    std::string strDst = req.getDestinationAccount().value_or("");
    auto const raDstAccount = [&]() -> std::optional<AccountID> {
        return strDst.empty() ? std::nullopt : parseBase58<AccountID>(strDst);
    }();
    if (!strDst.empty() && !raDstAccount)
        return Unexpected(
            AccountChannelsErrorResponseCodes::ACTNOTFOUND);  // todo: malformed

    unsigned int limit;
    if (auto err = readLimitField(
            limit, RPC::Tuning::accountChannels, context))  // context.params?
        return Unexpected(
            AccountChannelsErrorResponseCodes::
                INVALIDPARAMS);  // todo: should use err here somehow to get
                                 // message. or just rewrite readLimitField

    if (limit == 0u)
        return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);

    std::vector<Channel> channels;
    struct VisitData
    {
        std::vector<std::shared_ptr<SLE const>> items;
        AccountID const& accountID;
        std::optional<AccountID> const& raDstAccount;
    };
    VisitData visitData = {{}, accountID, raDstAccount};
    visitData.items.reserve(limit);
    uint256 startAfter = beast::zero;
    std::uint64_t startHint = 0;

    if (req.getMarker().has_value())
    {
        // Marker is composed of a comma separated index and start hint. The
        // former will be read as hex, and the latter using boost lexical cast.
        std::stringstream marker(req.getMarker().value());
        std::string value;
        if (!std::getline(marker, value, ','))
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);

        if (!startAfter.parseHex(value))
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);

        if (!std::getline(marker, value, ','))
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);

        try
        {
            startHint = boost::lexical_cast<std::uint64_t>(value);
        }
        catch (boost::bad_lexical_cast&)
        {
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);
        }

        // We then must check if the object pointed to by the marker is actually
        // owned by the account in the request.
        auto const sle = ledger->read({ltANY, startAfter});

        if (!sle)
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);

        if (!RPC::isRelatedToAccount(*ledger, sle, accountID))
            return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);
    }

    auto count = 0;
    std::optional<uint256> marker = {};
    std::uint64_t nextHint = 0;
    if (!forEachItemAfter(
            *ledger,
            accountID,
            startAfter,
            startHint,
            limit + 1,
            [&visitData, &accountID, &count, &limit, &marker, &nextHint](
                std::shared_ptr<SLE const> const& sleCur) {
                if (!sleCur)
                {
                    UNREACHABLE("ripple::doAccountChannels : null SLE");
                    return false;
                }

                if (++count == limit)
                {
                    marker = sleCur->key();
                    nextHint = RPC::getStartHint(sleCur, visitData.accountID);
                }

                if (count <= limit && sleCur->getType() == ltPAYCHAN &&
                    (*sleCur)[sfAccount] == accountID &&
                    (!visitData.raDstAccount ||
                     *visitData.raDstAccount == (*sleCur)[sfDestination]))
                {
                    visitData.items.emplace_back(sleCur);
                }

                return true;
            }))
    {
        return Unexpected(AccountChannelsErrorResponseCodes::INVALIDPARAMS);
    }

    auto resp = AccountChannelsSuccessResponse{};
    resp.setStatus(openapi_rippled::model::AccountChannelsSuccessResponseBase::
                       StatusEnum::SUCCESS);

    if (count == limit + 1 && marker)
    {
        resp.setLimit(limit);
        resp.setMarker(to_string(*marker) + "," + std::to_string(nextHint));
    }

    resp.setAccount(toBase58(accountID));

    for (auto const& item : visitData.items)
        addChannel(channels, *item);

    context.loadType = Resource::feeMediumBurdenRPC;
    resp.setChannels(std::move(channels));
    resp.setValidated(result["validated"].asBool());
    if (auto const& v = result["ledger_index"])
    {
        if (v.isString())
        {
            resp.setLedgerIndex(v.asString());
        }
        else
        {
            resp.setLedgerIndex(v.asUInt());
        }
    }
    if (auto const& v = result["ledger_hash"])
        resp.setLedgerHash(v.asString());

    return resp;
}

}  // namespace ripple
