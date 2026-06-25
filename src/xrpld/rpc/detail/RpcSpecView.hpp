/** @file */
#pragma once
// xrpl::json::Value adapters satisfying rpc::spec::SomeFieldView and SomeObjectView.
// Allows the rpcspec DSL (consteval spec framework) to validate xrpl::json::Value
// request parameters in rippled handlers.

#include <xrpld/rpc/Status.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

#include <rpcspec/Concepts.hpp>
#include <rpcspec/Errors.hpp>
#include <rpcspec/RpcSpec.hpp>
#include <rpcspec/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xrpl::RPC {

// Non-owning view of a single resolved field within a json::Value object.
// Holds either a mutable or const pointer to the stored node (nullptr = absent).
class XrplJsonFieldView
{
    json::Value const* readValue_;
    json::Value* writeValue_;
    std::string_view key_;

public:
    XrplJsonFieldView(json::Value* v, std::string_view k) noexcept
        : readValue_{v}, writeValue_{v}, key_{k}
    {
    }

    XrplJsonFieldView(json::Value const* v, std::string_view k) noexcept
        : readValue_{v}, writeValue_{nullptr}, key_{k}
    {
    }

    [[nodiscard]] std::string_view
    key() const noexcept
    {
        return key_;
    }

    [[nodiscard]] bool
    present() const noexcept
    {
        return readValue_ != nullptr;
    }

    // json::Value only has 32-bit int types; treat all integral values as int64-compatible.
    [[nodiscard]] bool
    isInt64() const noexcept
    {
        return readValue_ != nullptr && readValue_->isIntegral();
    }

    [[nodiscard]] int64_t
    asInt64() const
    {
        return readValue_->isInt() ? static_cast<int64_t>(readValue_->asInt())
                                   : static_cast<int64_t>(readValue_->asUInt());
    }

    [[nodiscard]] bool
    isUint32() const noexcept
    {
        return readValue_ != nullptr && readValue_->isUInt();
    }

    [[nodiscard]] uint32_t
    asUint32() const
    {
        return readValue_->asUInt();
    }

    [[nodiscard]] bool
    isBool() const noexcept
    {
        return readValue_ != nullptr && readValue_->isBool();
    }

    [[nodiscard]] bool
    asBool() const
    {
        return readValue_->asBool();
    }

    [[nodiscard]] bool
    isString() const noexcept
    {
        return readValue_ != nullptr && readValue_->isString();
    }

    // Returns std::string (not string_view) because json::Value::asString() returns by value.
    // Satisfies -> std::convertible_to<std::string_view> in SomeFieldView; callers using auto
    // will get an owned copy, avoiding any dangling-view risk.
    [[nodiscard]] std::string
    asString() const
    {
        return readValue_->asString();
    }

    [[nodiscard]] bool
    isDouble() const noexcept
    {
        return readValue_ != nullptr && readValue_->isDouble();
    }

    [[nodiscard]] double
    asDouble() const
    {
        return readValue_->asDouble();
    }

    [[nodiscard]] bool
    isObject() const noexcept
    {
        return readValue_ != nullptr && readValue_->isObject();
    }

    [[nodiscard]] bool
    isArray() const noexcept
    {
        return readValue_ != nullptr && readValue_->isArray();
    }

    [[nodiscard]] std::size_t
    arraySize() const noexcept
    {
        if (readValue_ == nullptr || !readValue_->isArray())
            return 0;
        return readValue_->size();
    }

    [[nodiscard]] XrplJsonFieldView
    child(std::string_view childKey) const  // std::string ctor can throw; not noexcept
    {
        std::string k{childKey};
        if (writeValue_ != nullptr && writeValue_->isObject() && writeValue_->isMember(k))
            return {&(*writeValue_)[k], childKey};
        if (readValue_ != nullptr && readValue_->isObject() && readValue_->isMember(k))
            return {&(*readValue_)[k], childKey};
        return {static_cast<json::Value const*>(nullptr), childKey};
    }

    [[nodiscard]] XrplJsonFieldView
    element(std::size_t idx) const noexcept
    {
        if (writeValue_ != nullptr && writeValue_->isArray() && idx < writeValue_->size())
            return {&(*writeValue_)[static_cast<json::UInt>(idx)], key_};
        if (readValue_ != nullptr && readValue_->isArray() && idx < readValue_->size())
            return {&(*readValue_)[static_cast<json::UInt>(idx)], key_};
        return {static_cast<json::Value const*>(nullptr), key_};
    }

    template <typename T>
    [[nodiscard]] bool
    is() const noexcept
    {
        if constexpr (std::is_same_v<T, int64_t>)
        {
            return isInt64();
        }
        else if constexpr (std::is_same_v<T, uint32_t>)
        {
            return isUint32();
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return isBool();
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            return isString();
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            return isDouble();
        }
        else if constexpr (std::is_same_v<T, rpc::spec::JsonObject>)
        {
            return isObject();
        }
        else if constexpr (std::is_same_v<T, rpc::spec::JsonArray>)
        {
            return isArray();
        }
        else
        {
            static_assert(false, "unsupported type for XrplJsonFieldView::is<T>()");
        }
    }

    void
    set(int64_t v)
    {
        *writeValue_ = static_cast<json::Int>(v);
    }

    void
    set(uint32_t v)
    {
        *writeValue_ = static_cast<json::UInt>(v);
    }

    void
    set(std::string_view v)
    {
        *writeValue_ = std::string{v};
    }

    void
    set(bool v)
    {
        *writeValue_ = v;
    }

    void
    set(double v)
    {
        *writeValue_ = v;
    }
};

static_assert(rpc::spec::SomeFieldView<XrplJsonFieldView>);

// Non-owning view of the request params object root (json::Value that must be an object).
class XrplJsonObjectView
{
    json::Value const* readValue_;
    json::Value* writeValue_;

public:
    explicit XrplJsonObjectView(json::Value& v) noexcept : readValue_{&v}, writeValue_{&v}
    {
    }

    explicit XrplJsonObjectView(json::Value const& v) noexcept
        : readValue_{&v}, writeValue_{nullptr}
    {
    }

    [[nodiscard]] bool
    isObject() const noexcept
    {
        return readValue_->isObject();
    }

    [[nodiscard]] bool
    isArray() const noexcept
    {
        return readValue_->isArray();
    }

    [[nodiscard]] XrplJsonFieldView
    child(std::string_view key)
    {
        std::string k{key};
        if (writeValue_ != nullptr && writeValue_->isObject() && writeValue_->isMember(k))
            return {&(*writeValue_)[k], key};
        return {static_cast<json::Value*>(nullptr), key};
    }

    [[nodiscard]] XrplJsonFieldView
    child(std::string_view key) const
    {
        std::string k{key};
        if (readValue_->isObject() && readValue_->isMember(k))
            return {&(*readValue_)[k], key};
        return {static_cast<json::Value const*>(nullptr), key};
    }
};

static_assert(rpc::spec::SomeObjectView<XrplJsonObjectView>);

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

// Type-erased spec view for rippled's json::Value backend.
// Mirrors rpc::spec::RpcSpecView (boost::json backend) but operates on XrplJsonObjectView.
// Handlers in rippled return this from their static spec() method; the framework calls
// process() before invoking check().
class XrplRpcSpecView
{
    void const* self_;
    rpc::spec::MaybeError (*processImpl_)(void const*, XrplJsonObjectView&);
    rpc::spec::Warnings (*checkImpl_)(void const*, XrplJsonObjectView const&);

public:
    template <typename... Fields>
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr XrplRpcSpecView(rpc::spec::RpcSpec<Fields...> const& spec) noexcept
        : self_{&spec}
        , processImpl_{[](void const* s, XrplJsonObjectView& r) -> rpc::spec::MaybeError {
            return static_cast<rpc::spec::RpcSpec<Fields...> const*>(s)->process(r);
        }}
        , checkImpl_{[](void const* s, XrplJsonObjectView const& r) -> rpc::spec::Warnings {
            return static_cast<rpc::spec::RpcSpec<Fields...> const*>(s)->check(r);
        }}
    {
    }

    [[nodiscard]] rpc::spec::MaybeError
    process(XrplJsonObjectView& root) const
    {
        return processImpl_(self_, root);
    }

    // Convenience overload: wraps a mutable json::Value directly.
    [[nodiscard]] rpc::spec::MaybeError
    process(json::Value& v) const
    {
        XrplJsonObjectView root{v};
        return processImpl_(self_, root);
    }

    // Warning phase: collects non-fatal warnings (e.g. deprecated fields).
    // Runs after a successful process(); never fails.
    [[nodiscard]] rpc::spec::Warnings
    check(XrplJsonObjectView const& root) const
    {
        return checkImpl_(self_, root);
    }

    [[nodiscard]] rpc::spec::Warnings
    check(json::Value const& v) const
    {
        XrplJsonObjectView const root{v};
        return checkImpl_(self_, root);
    }
};

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
