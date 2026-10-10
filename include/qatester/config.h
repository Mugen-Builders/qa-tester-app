// Runtime configuration (addresses, sizes). Read from environment variables by the driver and
// by tests; the engine never reads the environment itself.
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace qatester {

enum class DepositPolicy : uint8_t {
    CreditAll = 0,        // every portal deposit is accepted and credited (default)
    RejectNonToken = 1,   // deposits of anything but the configured token are rejected
    RejectAll = 2,        // every deposit is rejected (report emitted first)
};

struct Config {
    // Devnet defaults come from `cartesi address-book` (rollups-contracts v3.0.0-alpha.10).
    Address token;                 // the single withdrawable ERC-20 (accounts drive)
    Address ether_portal;
    Address erc20_portal;
    Address erc721_portal;
    Address erc1155_single_portal;
    Address erc1155_batch_portal;
    Address safe_erc20_transfer;   // default destination for withdrawErc20Safe (may be zero)
    DepositPolicy deposit_policy{DepositPolicy::CreditAll};
    bool log{true};                // stderr logging

    // Ledger layout inside the state drive (bytes). Region A is the single-asset ledger whose
    // first `single_accounts * 32` bytes are the accounts drive proved on-chain.
    size_t single_accounts{1u << 17};
    size_t region_a_length{32u << 20};
    size_t region_b_length{32u << 20};
    size_t multi_accounts{8u * 1024};   // 8192/64/65536 needs ~16 MiB of the 32 MiB region B
    size_t multi_assets{64};
    size_t multi_balances{64u * 1024};

    [[nodiscard]] static Config devnet_defaults();
    // Overrides from QA_* environment variables (see README); returns false with `err` set when a
    // variable is present but malformed.
    [[nodiscard]] bool apply_env(std::string &err);
    [[nodiscard]] std::string describe_json() const;
    [[nodiscard]] size_t accounts_drive_length() const { return single_accounts * 32; }
};

} // namespace qatester
