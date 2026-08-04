#pragma once

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/TxQ.h>  // IWYU pragma: keep
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/Status.h>
#include <xrpld/rpc/detail/Handler.h>

#include <xrpl/json/json_value.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/ApiVersion.h>

#include <rpcspec/HandlerFor.hpp>
#include <rpcspec/handlers/ledger/Types.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace json {
class Object;
}  // namespace json

namespace xrpl::RPC {

struct JsonContext;

// ledger [id|index|current|closed] [full]
// {
//    ledger: 'current' | 'closed' | <uint256> | <number>,  // optional
//    full: true | false    // optional, defaults to false.
// }

class LedgerHandler : public rpc::spec::HandlerFor<rpc::spec::handlers::ledger::Input>
{
public:
    struct Output
    {
        std::shared_ptr<ReadView const> ledger;
        std::vector<TxQ::TxDetails> queueTxs;
        int options = 0;
        json::Value result;
    };

    using Result = std::expected<Output, Status>;

    explicit LedgerHandler(JsonContext&);

    [[nodiscard]] Result
    process(Input const& input);

    void
    writeResult(json::Value&, Output const&);

    // NOLINTBEGIN(readability-identifier-naming)
    static constexpr char name[] = "ledger";

    static constexpr uint32_t minApiVer = RPC::kApiMinimumSupportedVersion;

    static constexpr uint32_t maxApiVer = RPC::kApiMaximumValidVersion;

    static constexpr Role role = Role::USER;

    static constexpr Condition condition = Condition::NoCondition;
    // NOLINTEND(readability-identifier-naming)

private:
    JsonContext& context_;
};

}  // namespace xrpl::RPC
