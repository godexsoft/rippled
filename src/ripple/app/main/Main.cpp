//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/main/DBInit.h>
#include <ripple/app/rdb/Vacuum.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/basics/contract.h>
#include <ripple/beast/clock/basic_seconds_clock.h>
#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/core/Config.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/core/TimeKeeper.h>
#include <ripple/json/to_string.h>
#include <ripple/net/RPCCall.h>
#include <ripple/protocol/BuildInfo.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/RPCHandler.h>

#include <ripple/app/ledger/InboundLedger.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/shamap/SHAMap.h>
#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <test/shamap/common.h>
#include <thread>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

struct LedgerObject
{
    std::string key;
    std::string object;
};

struct TransactionObject
{
    std::string tx_blob;
    std::string meta;
};

boost::json::object
request(
    std::string const& host,
    std::string const& port,
    boost::json::object const& request)
{
    try
    {
        boost::asio::io_context ioc;

        boost::asio::ip::tcp::resolver resolver(ioc);
        boost::beast::tcp_stream stream(ioc);

        auto const results = resolver.resolve(host, port);

        stream.connect(results);

        std::string target = "/";
        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::post);
        req.target("/");
        req.set(boost::beast::http::field::host, host);
        req.set(
            boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.body() = std::string(boost::json::serialize(request));
        req.prepare_payload();

        // Send the HTTP request to the remote host
        boost::beast::http::write(stream, req);

        // This buffer is used for reading and must be persisted
        boost::beast::flat_buffer buffer;

        // Declare a container to hold the response
        boost::beast::http::response<boost::beast::http::string_body> res;

        // Receive the HTTP response
        boost::beast::http::read(stream, buffer, res);

        // Gracefully close the socket
        boost::beast::error_code ec;
        stream.socket().shutdown(
            boost::asio::ip::tcp::socket::shutdown_both, ec);
        if (res.result() != boost::beast::http::status::ok)
        {
            std::stringstream msg;
            msg << "Response not ok. response = " << res.body();
            throw std::runtime_error(msg.str());
        }

        return boost::json::parse(res.body()).as_object();
    }
    catch (std::exception const& e)
    {
        std::stringstream msg;
        msg << "Error communicating with clio. host = " << host
            << ", port = " << port << ", request = " << request << ", e.what() "
            << e.what();
        std::cout << msg.str() << std::endl;

        throw std::runtime_error(msg.str());
    }
}

std::vector<LedgerObject>
loadInitialLedger(
    std::string const& host,
    std::string const& port,
    std::uint32_t ledger_index,
    bool outOfOrder)
{
    auto getRequest = [&](auto marker) {
        boost::json::object params = {
            {"ledger_index", ledger_index},
            {"binary", true},
            {"out_of_order", outOfOrder}};

        if (marker)
            params["marker"] = *marker;

        boost::json::object request = {};
        request["method"] = "ledger_data";

        request["params"] = boost::json::array{params};

        return request;
    };

    std::vector<LedgerObject> results = {};

    std::optional<boost::json::value> marker;
    do
    {
        auto req = getRequest(marker);
        auto response = request(host, port, req);
        if (!response.contains("result") ||
            response["result"].as_object().contains("error"))
        {
            std::cout << response << std::endl;
            continue;
        }

        auto result = response["result"].as_object();

        if (result.contains("marker"))
            marker = result.at("marker");
        else
            marker = {};

        boost::json::array& state = result["state"].as_array();

        for (auto const& ledgerObject : state)
        {
            boost::json::object const& obj = ledgerObject.as_object();

            LedgerObject stateObject = {};

            stateObject.key = obj.at("index").as_string().c_str();
            stateObject.object = obj.at("data").as_string().c_str();

            results.push_back(stateObject);
        }

    } while (marker);

    return results;
}

std::vector<TransactionObject>
fetchTransactions(
    std::string const& host,
    std::string const& port,
    std::uint32_t ledger_index)
{
    while (true)
    {
        boost::json::object params = {
            {"ledger_index", ledger_index},
            {"binary", true},
            {"expand", true},
            {"transactions", true},
        };

        boost::json::object req = {};
        req["method"] = "ledger";

        req["params"] = boost::json::array{params};

        auto response = request(host, port, req);
        if (!response.contains("result") ||
            response["result"].as_object().contains("error"))
        {
            std::cout << response << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        auto& result = response["result"].as_object();
        auto& ledger = result["ledger"].as_object();

        boost::json::array& txs = ledger["transactions"].as_array();

        std::vector<TransactionObject> results;
        for (auto const& tx : txs)
        {
            boost::json::object const& obj = tx.as_object();

            TransactionObject txObject;
            txObject.tx_blob = obj.at("tx_blob").as_string().c_str();
            txObject.meta = obj.at("meta").as_string().c_str();

            results.push_back(txObject);
        }

        return results;
    }
}

std::tuple<
    ripple::LedgerInfo,
    std::vector<TransactionObject>,
    std::vector<LedgerObject>>
fetchTransactionsAndDiff(
    std::string const& host,
    std::string const& port,
    std::uint32_t ledger_index)
{
    while (true)
    {
        boost::json::object params = {
            {"ledger_index", ledger_index},
            {"binary", true},
            {"expand", true},
            {"transactions", true},
            {"diff", true}};

        boost::json::object req = {};
        req["method"] = "ledger";

        req["params"] = boost::json::array{params};

        auto response = request(host, port, req);
        if (!response.contains("result") ||
            response["result"].as_object().contains("error"))
        {
            std::cout << response << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        auto& result = response["result"].as_object();
        auto& ledger = result["ledger"].as_object();

        boost::json::array& txs = ledger["transactions"].as_array();

        std::vector<TransactionObject> txResults;
        for (auto const& tx : txs)
        {
            boost::json::object const& obj = tx.as_object();

            TransactionObject txObject;
            txObject.tx_blob = obj.at("tx_blob").as_string().c_str();
            txObject.meta = obj.at("meta").as_string().c_str();

            txResults.push_back(txObject);
        }

        boost::json::array& diff = ledger["diff"].as_array();

        std::vector<LedgerObject> diffResults = {};
        for (auto const& state : diff)
        {
            boost::json::object const& obj = state.as_object();

            LedgerObject ledgerObject;
            std::string key = obj.contains("id") ? "id" : "object_id";
            ledgerObject.key = obj.at(key).as_string().c_str();
            ledgerObject.object = obj.at("object").as_string().c_str();

            diffResults.push_back(ledgerObject);
        }

        std::string ledgerData = ledger["ledger_data"].as_string().c_str();
        auto lgrBlob = ripple::strUnHex(ledgerData);
        ripple::LedgerInfo header =
            ripple::deserializeHeader(ripple::makeSlice(*lgrBlob));

        return {header, txResults, diffResults};
    }
}

ripple::uint256
hashTransactions(std::vector<TransactionObject> const& txs)
{
    ripple::tests::TestNodeFamily f{
        beast::Journal{beast::Journal::getNullSink()}};

    ripple::SHAMap txMap = ripple::SHAMap(ripple::SHAMapType::TRANSACTION, f);

    for (auto& tx : txs)
    {
        auto txn = ripple::strUnHex(tx.tx_blob);
        auto metaData = ripple::strUnHex(tx.meta);

        ripple::SerialIter sit(ripple::Slice{txn->data(), txn->size()});
        ripple::STTx sttx{sit};

        ripple::Serializer s(txn->size() + metaData->size() + 16);
        s.addVL(*txn);
        s.addVL(*metaData);

        auto item = ripple::make_shamapitem(sttx.getTransactionID(), s.slice());
        txMap.addItem(
            ripple::SHAMapNodeType::tnTRANSACTION_MD, std::move(item));
    }

    return txMap.getHash().as_uint256();
}

void
addToSHAMap(ripple::SHAMap& shamap, LedgerObject const& obj)
{
    auto keyBlob = ripple::strUnHex(obj.key);
    ripple::uint256 key = ripple::uint256::fromVoid(keyBlob->data());
    auto blob = ripple::strUnHex(obj.object);

    if (!blob)
        throw std::runtime_error("INVALID PARSE HEX");

    auto slice = ripple::Slice{blob->data(), blob->size()};
    auto item = ripple::make_shamapitem(key, std::move(slice));
    shamap.addItem(ripple::SHAMapNodeType::tnACCOUNT_STATE, std::move(item));
}

void
removeFromSHAMap(ripple::SHAMap& shamap, ripple::uint256 const& key)
{
    shamap.delItem(key);
}

void
updateSHAMapItem(ripple::SHAMap& shamap, LedgerObject const& obj)
{
    auto keyBlob = ripple::strUnHex(obj.key);
    ripple::uint256 key = ripple::uint256::fromVoid(keyBlob->data());
    auto blob = ripple::strUnHex(obj.object);

    if (!blob)
        throw std::runtime_error("INVALID PARSE HEX");

    auto slice = ripple::Slice{blob->data(), blob->size()};
    auto item = ripple::make_shamapitem(key, slice);

    shamap.updateGiveItem(
        ripple::SHAMapNodeType::tnACCOUNT_STATE, std::move(item));
}

std::uint32_t
getMostRecent(std::string const& url, std::string const& port)
{
    boost::json::object params = {{"ledger_index", "validated"}};

    boost::json::object req = {};
    req["method"] = "ledger";

    req["params"] = boost::json::array{params};

    auto response = request(url, port, req);

    return response["result"].as_object()["ledger_index"].as_int64();
}

int
main(int argc, char* argv[])
{
    if (argc != 4 && argc != 5 && argc != 6)
    {
        std::cout << "Args are invalid: usage ./rippled <startingSequence> "
                     "<URL> <port> [use_cache] [allow_missing_transactions]";
    }

    std::uint32_t startingSequence =
        boost::lexical_cast<std::uint32_t>(argv[1]);

    std::string const url = argv[2];
    std::string const port = argv[3];

    bool outOfOrder = true;
    if (argc >= 5)
        outOfOrder = (std::string{argv[4]} != "false");

    bool allowMissingTxns = false;
    if (argc >= 6)
        allowMissingTxns = true;

    if (startingSequence == 0)
        startingSequence = getMostRecent(url, port);

    std::thread successorVerifier([&]() {
        while (true)
        {
            auto mostRecent = getMostRecent(url, port);

            ripple::tests::TestNodeFamily f{
                beast::Journal{beast::Journal::getNullSink()}};

            ripple::SHAMap stateMap =
                ripple::SHAMap(ripple::SHAMapType::STATE, f);

            auto state = loadInitialLedger(url, port, mostRecent, outOfOrder);

            for (auto const& obj : state)
                addToSHAMap(stateMap, obj);

            auto [lgrInfo, firstTxs, _] =
                fetchTransactionsAndDiff(url, port, mostRecent);

            if (lgrInfo.accountHash != stateMap.getHash().as_uint256())
            {
                std::cout << "FAILED TO VERIFY SUCCESSORS AT LEDGER: "
                          << lgrInfo.seq << std::endl;
                throw std::runtime_error(
                    "FAILED TO VERIFY SUCCESSORS AT LEDGER: " +
                    std::to_string(lgrInfo.seq));
            }
            else
            {
                std::cout << "VERIFIED SUCCESSORS AT LEDGER: " << lgrInfo.seq
                          << std::endl;
            }
        }
    });

    ripple::tests::TestNodeFamily f{
        beast::Journal{beast::Journal::getNullSink()}};

    ripple::SHAMap stateMap = ripple::SHAMap(ripple::SHAMapType::STATE, f);

    auto state = loadInitialLedger(url, port, startingSequence, outOfOrder);

    std::cout << "INITIAL LEDGER STATE IS SIZE: " << state.size() << std::endl;

    for (auto const& obj : state)
        addToSHAMap(stateMap, obj);

    auto [lgrInfo, firstTxs, _] =
        fetchTransactionsAndDiff(url, port, startingSequence);

    auto stateHash = stateMap.getHash().as_uint256();
    auto transactionHash = hashTransactions(firstTxs);

    if (lgrInfo.txHash != transactionHash)
        throw std::runtime_error(
            "COULD NOT VERIFY INITAL LEDGER TX " +
            std::to_string(startingSequence));

    if (lgrInfo.accountHash != stateHash)
        throw std::runtime_error(
            "COULD NOT VERIFY INITAL ACCOUNT HASH " +
            std::to_string(startingSequence));

    std::cout << "VERIFIED LEDGER: " << std::to_string(startingSequence)
              << std::endl;

    std::thread dataVerifier([&]() {
        while (true)
        {
            ++startingSequence;
            auto [header, txs, diffs] =
                fetchTransactionsAndDiff(url, port, startingSequence);

            transactionHash = hashTransactions(txs);

            for (auto const& diff : diffs)
            {
                auto keyBlob = ripple::strUnHex(diff.key);
                ripple::uint256 key =
                    ripple::uint256::fromVoid(keyBlob->data());

                if (diff.object.size() == 0)
                    removeFromSHAMap(stateMap, key);
                else if (stateMap.hasItem(key))
                    updateSHAMapItem(stateMap, diff);
                else
                    addToSHAMap(stateMap, diff);
            }

            auto stateHash = stateMap.getHash().as_uint256();
            if (header.txHash != transactionHash)
            {
                std::cout << "FAILED TO VERIFY LEDGER! Txn hash mismatch: "
                          << std::to_string(startingSequence) << std::endl;
                if (!allowMissingTxns)
                    throw std::runtime_error(
                        "FAILED TO VERIFY LEDGER " +
                        std::to_string(startingSequence));
            }
            if (header.accountHash != stateHash)
            {
                std::cout << "FAILED TO VERIFY LEDGER! state hash mismatch: "
                          << std::to_string(startingSequence) << std::endl;
                throw std::runtime_error(
                    "FAILED TO VERIFY LEDGER " +
                    std::to_string(startingSequence));
            }

            std::cout << "VERIFIED LEDGER: " << std::to_string(startingSequence)
                      << std::endl;
        }
    });
    successorVerifier.join();
    dataVerifier.join();

    return 0;
}
