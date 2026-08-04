#pragma once

#include <xrpld/app/main/Application.h>  // IWYU pragma: keep
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/Status.h>
#include <xrpld/rpc/detail/Handler.h>

#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ApiVersion.h>

#include <cstdint>
#include <expected>

namespace xrpl::RPC {

// 'version' takes no parameters, so it is a plain (non-spec) handler: the RPC
// framework default-constructs its empty Input rather than parsing one from a spec.
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

    void
    writeResult(json::Value& obj, [[maybe_unused]] Output const&) const
    {
        setVersion(obj, apiVersion_, betaEnabled_);
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
