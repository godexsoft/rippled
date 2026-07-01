#pragma once

/** @file */
#pragma once
// Bridging helpers between rippled's json::Value RPC layer and the rpcspec DSL.
//
// The spec framework validates + parses requests over boost::json (its `parseInput`
// and `check` are boost::json-only). rippled's request params are xrpl::json::Value,
// so a handler's dispatch converts them with toBoostJson() before handing them to the
// spec, then maps any resulting rpc::Status back to a rippled RPC Status and appends
// spec-produced warnings in rippled's wire format.

#include <xrpld/rpc/Status.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <rpcspec/Errors.hpp>
#include <rpcspec/Types.hpp>

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace xrpl::RPC {

// Recursively convert an xrpl::json::Value into a boost::json::value so the rpcspec
// DSL can validate and parse it. json::Value distinguishes signed/unsigned integers,
// which are preserved as int64/uint64 respectively.
[[nodiscard]] inline boost::json::value
toBoostJson(json::Value const& v)
{
    switch (v.type())
    {
        case json::ValueType::Null:
            return nullptr;
        case json::ValueType::Int:
            return boost::json::value{static_cast<std::int64_t>(v.asInt())};
        case json::ValueType::UInt:
            return boost::json::value{static_cast<std::uint64_t>(v.asUInt())};
        case json::ValueType::Real:
            return boost::json::value{v.asDouble()};
        case json::ValueType::String:
            return boost::json::value{v.asString()};
        case json::ValueType::Boolean:
            return boost::json::value{v.asBool()};
        case json::ValueType::Array: {
            boost::json::array arr;
            arr.reserve(v.size());
            for (json::UInt i = 0; i < v.size(); ++i)
                arr.push_back(toBoostJson(v[i]));
            return arr;
        }
        case json::ValueType::Object: {
            boost::json::object obj;
            for (auto const& name : v.getMemberNames())
                obj[name] = toBoostJson(v[name]);
            return obj;
        }
    }
    return nullptr;  // LCOV_EXCL_LINE -- all ValueType cases handled above
}

// Converts a spec DSL error (rpc::Status) to a rippled RPC status.
// RippledError maps directly; ClioError/EtlError (shouldn't occur in rippled) fall back to
// RpcInvalidParams.
[[nodiscard]] inline xrpl::RPC::Status
toRippleStatus(rpc::Status const& s)
{
    if (auto const* err = std::get_if<rpc::RippledError>(&s.code))
    {
        if (!s.message.empty())
            return xrpl::RPC::Status{*err, s.message};
        return xrpl::RPC::Status{*err};
    }
    return xrpl::RPC::Status{xrpl::RpcInvalidParams};
}

// Appends spec-produced warnings to obj[jss::warnings] in rippled's wire format.
// Warnings are grouped by code (one JSON object per code): the standard message
// for the code is followed by each warning's per-field detail. Existing entries
// in obj[jss::warnings] are preserved.
inline void
injectSpecWarnings(json::Value& obj, rpc::spec::Warnings const& warnings)
{
    if (warnings.empty())
        return;

    std::map<rpc::WarningCode, std::vector<std::string>> grouped;
    for (auto const& w : warnings)
        grouped[w.code].push_back(w.message);

    json::Value& arr = obj.isMember(jss::warnings)
        ? obj[jss::warnings]
        : (obj[jss::warnings] = json::Value{json::ValueType::Array});

    for (auto const& [code, messages] : grouped)
    {
        json::Value& w = arr.append(json::Value{json::ValueType::Object});
        w[jss::id] = static_cast<int>(code);
        std::string message{rpc::getWarningInfo(code).message};
        for (auto const& extra : messages)
        {
            message += ' ';
            message += extra;
        }
        w[jss::message] = message;
    }
}

}  // namespace xrpl::RPC
