#pragma once

#include <xrpld/rpc/detail/RpcSpecView.hpp>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ApiVersion.h>

#include <rpcspec/RpcSpec.hpp>

#include <cstdint>
#include <expected>

namespace xrpl::RPC {

class VersionHandler
{
public:
    struct Input
    {
    };
    struct Output
    {
    };

    using Result = std::expected<Output, Status>;

    explicit VersionHandler(JsonContext& c)
        : apiVersion_(static_cast<uint32_t>(c.apiVersion)), betaEnabled_(c.app.config().betaRpcApi)
    {
    }

    [[nodiscard]] Result
    process([[maybe_unused]] Input const&) const
    {
        return Output{};
    }

    static Input
    readInput([[maybe_unused]] json::Value const&)
    {
        return {};
    }

    void
    writeResult(json::Value& obj, [[maybe_unused]] Output const&) const
    {
        setVersion(obj, apiVersion_, betaEnabled_);
    }

    static XrplRpcSpecView
    spec([[maybe_unused]] uint32_t)
    {
        static constexpr rpc::spec::RpcSpec<> kSpec{};
        return kSpec;
    }

    // NOLINTBEGIN(readability-identifier-naming)
    static constexpr char const* name = "version";

    static constexpr uint32_t minApiVer = RPC::kApiMinimumSupportedVersion;

    static constexpr uint32_t maxApiVer = RPC::kApiMaximumValidVersion;

    static constexpr Role role = Role::USER;

    static constexpr Condition condition = Condition::NoCondition;
    // NOLINTEND(readability-identifier-naming)

private:
    uint32_t apiVersion_;
    bool betaEnabled_;
};

}  // namespace xrpl::RPC
