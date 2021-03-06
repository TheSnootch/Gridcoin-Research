// Copyright (c) 2014-2020 The Gridcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "gridcoin/appcache.h"
#include "gridcoin/claim.h"
#include "gridcoin/contract/contract.h"
#include "gridcoin/contract/handler.h"
#include "gridcoin/beacon.h"
#include "gridcoin/project.h"
#include "gridcoin/researcher.h"
#include "gridcoin/support/block_finder.h"
#include "gridcoin/support/xml.h"
#include "gridcoin/tx_message.h"
#include "gridcoin/voting/payloads.h"
#include "gridcoin/voting/registry.h"
#include "util.h"
#include "wallet/wallet.h"

using namespace GRC;

namespace {
//!
//! \brief An empty, invalid contract payload.
//!
//! Useful for situations where we need to satisfy the interface but cannot
//! provide a valid contract payload.
//!
class EmptyPayload : public IContractPayload
{
public:
    GRC::ContractType ContractType() const override
    {
        return GRC::ContractType::UNKNOWN;
    }

    bool WellFormed(const ContractAction action) const override
    {
        return false;
    }

    std::string LegacyKeyString() const override
    {
        return "";
    }

    std::string LegacyValueString() const override
    {
        return "";
    }

    int64_t RequiredBurnAmount() const override
    {
        return MAX_MONEY;
    }

    ADD_CONTRACT_PAYLOAD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(
        Stream& s,
        Operation ser_action,
        const ContractAction contract_action)
    {
        return;
    }
}; // EmptyPayload

//!
//! \brief A payload parsed from a legacy, version 1 contract.
//!
//! Version 2+ contracts provide support for binary representation of payload
//! data. Legacy contract data exists as strings. This class provides for use
//! of the contract payload API with legacy string contracts.
//!
class LegacyPayload : public IContractPayload
{
public:
    std::string m_key;   //!< Legacy representation of a contract key.
    std::string m_value; //!< Legacy representation of a contract value.

    //!
    //! \brief Initialize an empty, invalid legacy payload.
    //!
    LegacyPayload()
    {
    }

    //!
    //! \brief Initialize a legacy payload with data from a legacy contract.
    //!
    //! \param key   Legacy contract key as it exists in a transaction.
    //! \param value Legacy contract value as it exists in a transaction.
    //!
    LegacyPayload(std::string key, std::string value)
        : m_key(std::move(key))
        , m_value(std::move(value))
    {
    }

    GRC::ContractType ContractType() const override
    {
        return GRC::ContractType::UNKNOWN;
    }

    bool WellFormed(const ContractAction action) const override
    {
        return !m_key.empty()
            && (action == ContractAction::REMOVE || !m_value.empty());
    }

    std::string LegacyKeyString() const override
    {
        return m_key;
    }

    std::string LegacyValueString() const override
    {
        return m_value;
    }

    int64_t RequiredBurnAmount() const override
    {
        return Contract::STANDARD_BURN_AMOUNT;
    }

    ADD_CONTRACT_PAYLOAD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(
        Stream& s,
        Operation ser_action,
        const ContractAction contract_action)
    {
        READWRITE(m_key);

        if (contract_action != ContractAction::REMOVE) {
            READWRITE(m_value);
        }
    }
}; // LegacyPayload

//!
//! \brief Temporary interface implementation that reads and writes contracts
//! to AppCache to use while we refactor away each of the AppCache sections:
//!
class AppCacheContractHandler : public IContractHandler
{
public:
    void Reset() override
    {
        ClearCache(Section::PROTOCOL);
        ClearCache(Section::SCRAPER);
    }

    bool Validate(const Contract& contract, const CTransaction& tx) const override
    {
        return true; // No contextual validation needed yet
    }

    void Add(const ContractContext& ctx) override
    {
        const auto payload = ctx->SharePayloadAs<LegacyPayload>();

        WriteCache(
            StringToSection(ctx->m_type.ToString()),
            payload->m_key,
            payload->m_value,
            ctx.m_tx.nTime);
    }

    void Delete(const ContractContext& ctx) override
    {
        const auto payload = ctx->SharePayloadAs<LegacyPayload>();

        DeleteCache(
            StringToSection(ctx->m_type.ToString()),
            payload->m_key);
    }
};

//!
//! \brief Handles unknown contract message types by logging a message.
//!
class UnknownContractHandler : public IContractHandler
{
public:
    void Reset() override
    {
        // Nothing to do.
    }

    bool Validate(const Contract& contract, const CTransaction& tx) const override
    {
        return true; // No contextual validation needed yet
    }

    //!
    //! \brief Handle a contract addition.
    //!
    //! \param ctx References the contract and associated context.
    //!
    void Add(const ContractContext& ctx) override
    {
        ctx->Log("WARNING: Add unknown contract type ignored");
    }

    //!
    //! \brief Handle a contract deletion.
    //!
    //! \param ctx References the contract and associated context.
    //!
    void Delete(const ContractContext& ctx) override
    {
        ctx->Log("WARNING: Delete unknown contract type ignored");
    }

    //!
    //! \brief Handle a contract reversal.
    //!
    //! \param ctx References the contract and associated context.
    //!
    void Revert(const ContractContext& ctx) override
    {
        ctx->Log("WARNING: Revert unknown contract type ignored");
    }
};

//!
//! \brief Processes contracts from transaction messages by routing them to the
//! appropriate contract handler implementations.
//!
class Dispatcher
{
public:
    //!
    //! \brief Reset the cached state of each contract handler to prepare for
    //! historical contract replay.
    //!
    void ResetHandlers()
    {
        GetBeaconRegistry().Reset();
        GetPollRegistry().Reset();
        GetWhitelist().Reset();
        m_appcache_handler.Reset();
    }

    //!
    //! \brief Validate the provided contract and forward it to the appropriate
    //! contract handler.
    //!
    //! \param ctx References the contract and associated context.
    //!
    void Apply(const ContractContext& ctx)
    {
        if (ctx->m_action == ContractAction::ADD) {
            ctx->Log("INFO: Add contract");
            GetHandler(ctx->m_type.Value()).Add(ctx);
            return;
        }

        if (ctx->m_action == ContractAction::REMOVE) {
            ctx->Log("INFO: Delete contract");
            GetHandler(ctx->m_type.Value()).Delete(ctx);
            return;
        }

        ctx.m_contract.Log("WARNING: Unknown contract action ignored");
    }

    //!
    //! \brief Perform contextual validation for the provided contract.
    //!
    //! \param contract Contract to validate.
    //! \param tx       Transaction that contains the contract.
    //!
    //! \return \c false If the contract fails validation.
    //!
    bool Validate(const Contract& contract, const CTransaction& tx)
    {
        return GetHandler(contract.m_type.Value()).Validate(contract, tx);
    }

    //!
    //! \brief Revert a previously-applied contract from a transaction message
    //! by passing it to the appropriate contract handler.
    //!
    //! \param ctx References the contract and associated context.
    //!
    void Revert(const ContractContext& ctx)
    {
        ctx->Log("INFO: Revert contract");

        // The default implementation of IContractHandler reverses an action
        // (addition or deletion) declared in the contract argument, but the
        // type-specific handlers may override this behavior as needed:
        GetHandler(ctx->m_type.Value()).Revert(ctx);
    }

private:
    AppCacheContractHandler m_appcache_handler; //<! Temporary.
    UnknownContractHandler m_unknown_handler;   //<! Logs unknown types.

    //!
    //! \brief Select an appropriate contract handler based on the message type.
    //!
    //! \param type Contract type. Determines how to handle the message.
    //!
    //! \return Reference to an object capable of handling the contract type.
    //!
    IContractHandler& GetHandler(const ContractType type)
    {
        // TODO: build contract handlers for the remaining contract types:
        // TODO: refactor to dynamic registration for easier testing:
        switch (type) {
            case ContractType::BEACON:     return GetBeaconRegistry();
            case ContractType::POLL:       return GetPollRegistry();
            case ContractType::PROJECT:    return GetWhitelist();
            case ContractType::PROTOCOL:   return m_appcache_handler;
            case ContractType::SCRAPER:    return m_appcache_handler;
            case ContractType::VOTE:       return GetPollRegistry();
            default:                       return m_unknown_handler;
        }
    }
}; // class Dispatcher

//!
//! \brief Global contract dispatcher instance.
//!
Dispatcher g_dispatcher;

} // anonymous namespace

// -----------------------------------------------------------------------------
// Global Functions
// -----------------------------------------------------------------------------

Contract GRC::MakeLegacyContract(
    const ContractType type,
    const ContractAction action,
    std::string key,
    std::string value)
{
    Contract contract = MakeContract<LegacyPayload>(
        action,
        std::move(key),
        std::move(value));

    contract.m_type = type;

    return contract;
}

void GRC::ReplayContracts(const CBlockIndex* pindex)
{
    static BlockFinder blockFinder;
    pindex = blockFinder.FindByMinTime(pindex->nTime - Beacon::MAX_AGE);

    LogPrint(BCLog::LogFlags::CONTRACT,
        "Replaying contracts from block %" PRId64 "...", pindex->nHeight);

    if (pindex->nHeight < (fTestNet ? 1 : 164618)) {
       return;
    }

    g_dispatcher.ResetHandlers();

    CBlock block;

    // These are memorized consecutively in order from oldest to newest.
    for (; pindex; pindex = pindex->pnext) {
        if (pindex->nIsContract == 1) {
            if (!block.ReadFromDisk(pindex)) {
                continue;
            }

            bool unused;
            ApplyContracts(block, pindex, unused);
        }

        if (pindex->nIsSuperBlock == 1 && pindex->nVersion >= 11) {
            if (block.hashPrevBlock != pindex->pprev->GetBlockHash()
                && !block.ReadFromDisk(pindex))
            {
                continue;
            }

            GetBeaconRegistry().ActivatePending(
                block.GetSuperblock()->m_verified_beacons.m_verified,
                block.GetBlockTime());
        }
    }

    Researcher::Refresh();
}

void GRC::ApplyContracts(
    const CBlock& block,
    const CBlockIndex* const pindex,
    bool& out_found_contract)
{
    out_found_contract = false;

    // Skip coinbase and coinstake transactions:
    for (auto iter = std::next(block.vtx.begin(), 2), end = block.vtx.end();
        iter != end;
        ++iter)
    {
        ApplyContracts(*iter, pindex, out_found_contract);
    }
}

void GRC::ApplyContracts(
    const CTransaction& tx,
    const CBlockIndex* const pindex,
    bool& out_found_contract)
{
    for (const auto& contract : tx.GetContracts()) {
        // V2 contract signatures are checked upon receipt:
        if (contract.m_version == 1 && !contract.Validate()) {
            continue;
        }

        // Support dynamic team requirement or whitelist configuration:
        //
        // TODO: move this into the appropriate contract handler.
        //
        if (contract.m_type == ContractType::PROTOCOL) {
            const ContractPayload payload = contract.m_body.AssumeLegacy();
            const std::string key = payload->LegacyKeyString();

            if (key == "REQUIRE_TEAM_WHITELIST_MEMBERSHIP"
                || key == "TEAM_WHITELIST")
            {
                // Rescan in-memory project CPIDs to resolve a primary CPID
                // that fits the now active team requirement settings:
                Researcher::MarkDirty();
            }
        }

        g_dispatcher.Apply({ contract, tx, pindex });

        // Don't track transaction message contracts in the block index:
        out_found_contract |= contract.m_type != ContractType::MESSAGE;
    }
}

bool GRC::ValidateContracts(const CTransaction& tx)
{
    for (const auto& contract : tx.GetContracts()) {
        if (!g_dispatcher.Validate(contract, tx)) {
            return false;
        }
    }

    return true;
}

void GRC::RevertContracts(const CTransaction& tx, const CBlockIndex* const pindex)
{
    // Reverse the contracts. Reorganize will load any previous versions:
    for (const auto& contract : tx.GetContracts()) {
        // V2 contract signatures are checked upon receipt:
        if (contract.m_version == 1 && !contract.VerifySignature()) {
            continue;
        }

        g_dispatcher.Revert({ contract, tx, pindex });
    }
}

// -----------------------------------------------------------------------------
// Class: Contract
// -----------------------------------------------------------------------------

constexpr int64_t Contract::STANDARD_BURN_AMOUNT; // for clang

Contract::Contract()
    : m_version(Contract::CURRENT_VERSION)
    , m_type(ContractType::UNKNOWN)
    , m_action(ContractAction::UNKNOWN)
    , m_body()
    , m_signature()
    , m_public_key()
{
}

Contract::Contract(
    Contract::Type type,
    Contract::Action action,
    Contract::Body body)
    : m_version(Contract::CURRENT_VERSION)
    , m_type(type)
    , m_action(action)
    , m_body(std::move(body))
    , m_signature()
    , m_public_key()
{
}

Contract::Contract(
    int version,
    Contract::Type type,
    Contract::Action action,
    Contract::Body body,
    Contract::Signature signature,
    Contract::PublicKey public_key)
    : m_version(version)
    , m_type(type)
    , m_action(action)
    , m_body(std::move(body))
    , m_signature(std::move(signature))
    , m_public_key(std::move(public_key))
{
}

const CPubKey& Contract::MessagePublicKey()
{
    // If the message key changes, add a conditional entry to this method that
    // returns the new key for the appropriate height.

    // 044b2938fbc38071f24bede21e838a0758a52a0085f2e034e7f971df445436a25
    // 2467f692ec9c5ba7e5eaa898ab99cbd9949496f7e3cafbf56304b1cc2e5bdf06e
    static const CPubKey since_block_0 ({
        0x04, 0x4b, 0x29, 0x38, 0xfb, 0xc3, 0x80, 0x71, 0xf2, 0x4b, 0xed,
        0xe2, 0x1e, 0x83, 0x8a, 0x07, 0x58, 0xa5, 0x2a, 0x00, 0x85, 0xf2,
        0xe0, 0x34, 0xe7, 0xf9, 0x71, 0xdf, 0x44, 0x54, 0x36, 0xa2, 0x52,
        0x46, 0x7f, 0x69, 0x2e, 0xc9, 0xc5, 0xba, 0x7e, 0x5e, 0xaa, 0x89,
        0x8a, 0xb9, 0x9c, 0xbd, 0x99, 0x49, 0x49, 0x6f, 0x7e, 0x3c, 0xaf,
        0xbf, 0x56, 0x30, 0x4b, 0x1c, 0xc2, 0xe5, 0xbd, 0xf0, 0x6e
    });

    return since_block_0;
}

const CPrivKey& Contract::MessagePrivateKey()
{
    // If the message key changes, add a conditional entry to this method that
    // returns the new key for the appropriate height.

    // 308201130201010420fbd45ffb02ff05a3322c0d77e1e7aea264866c24e81e5ab
    // 6a8e150666b4dc6d8a081a53081a2020101302c06072a8648ce3d0101022100ff
    // fffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f300
    // 604010004010704410479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28
    // d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a685541
    // 99c47d08ffb10d4b8022100fffffffffffffffffffffffffffffffebaaedce6af
    // 48a03bbfd25e8cd0364141020101a144034200044b2938fbc38071f24bede21e8
    // 38a0758a52a0085f2e034e7f971df445436a252467f692ec9c5ba7e5eaa898ab9
    // 9cbd9949496f7e3cafbf56304b1cc2e5bdf06e
    static const CPrivKey since_block_0 {
        0x30, 0x82, 0x01, 0x13, 0x02, 0x01, 0x01, 0x04, 0x20, 0xfb, 0xd4,
        0x5f, 0xfb, 0x02, 0xff, 0x05, 0xa3, 0x32, 0x2c, 0x0d, 0x77, 0xe1,
        0xe7, 0xae, 0xa2, 0x64, 0x86, 0x6c, 0x24, 0xe8, 0x1e, 0x5a, 0xb6,
        0xa8, 0xe1, 0x50, 0x66, 0x6b, 0x4d, 0xc6, 0xd8, 0xa0, 0x81, 0xa5,
        0x30, 0x81, 0xa2, 0x02, 0x01, 0x01, 0x30, 0x2c, 0x06, 0x07, 0x2a,
        0x86, 0x48, 0xce, 0x3d, 0x01, 0x01, 0x02, 0x21, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xfc, 0x2f, 0x30, 0x06, 0x04,
        0x01, 0x00, 0x04, 0x01, 0x07, 0x04, 0x41, 0x04, 0x79, 0xbe, 0x66,
        0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95, 0xce, 0x87,
        0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59,
        0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98, 0x48, 0x3a, 0xda, 0x77,
        0x26, 0xa3, 0xc4, 0x65, 0x5d, 0xa4, 0xfb, 0xfc, 0x0e, 0x11, 0x08,
        0xa8, 0xfd, 0x17, 0xb4, 0x48, 0xa6, 0x85, 0x54, 0x19, 0x9c, 0x47,
        0xd0, 0x8f, 0xfb, 0x10, 0xd4, 0xb8, 0x02, 0x21, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
        0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41, 0x02, 0x01, 0x01,
        0xa1, 0x44, 0x03, 0x42, 0x00, 0x04, 0x4b, 0x29, 0x38, 0xfb, 0xc3,
        0x80, 0x71, 0xf2, 0x4b, 0xed, 0xe2, 0x1e, 0x83, 0x8a, 0x07, 0x58,
        0xa5, 0x2a, 0x00, 0x85, 0xf2, 0xe0, 0x34, 0xe7, 0xf9, 0x71, 0xdf,
        0x44, 0x54, 0x36, 0xa2, 0x52, 0x46, 0x7f, 0x69, 0x2e, 0xc9, 0xc5,
        0xba, 0x7e, 0x5e, 0xaa, 0x89, 0x8a, 0xb9, 0x9c, 0xbd, 0x99, 0x49,
        0x49, 0x6f, 0x7e, 0x3c, 0xaf, 0xbf, 0x56, 0x30, 0x4b, 0x1c, 0xc2,
        0xe5, 0xbd, 0xf0, 0x6e
    };

    return since_block_0;
}

bool Contract::Detect(const std::string& message)
{
    return !message.empty()
        && Contains(message, "<MT>")
        // Superblock handled elsewhere:
        && !Contains(message, "<MT>superblock</MT>");
}

Contract Contract::Parse(const std::string& message)
{
    if (message.empty()) {
        return Contract();
    }

    return Contract(
        1, // Legacy XML-like string contracts always parse to a v1 contract.
        Contract::Type::Parse(ExtractXML(message, "<MT>", "</MT>")),
        Contract::Action::Parse(ExtractXML(message, "<MA>", "</MA>")),
        Contract::Body(ContractPayload::Make<LegacyPayload>(
            ExtractXML(message, "<MK>", "</MK>"),
            ExtractXML(message, "<MV>", "</MV>"))),
        Contract::Signature::Parse(ExtractXML(message, "<MS>", "</MS>")),
        // None of the currently-valid contract types support signing with a
        // user-supplied private key, so we can skip parsing the public keys
        // altogether. We verify contracts with the master and message keys:
        //Contract::PublicKey::Parse(ExtractXML(message, "<MPK>", "</MPK>")),
        Contract::PublicKey());
}

bool Contract::RequiresMasterKey() const
{
    switch (m_type.Value()) {
        case ContractType::BEACON:
            // Contracts version 2+ allow participants to revoke their own
            // beacons by signing them with the original private key:
            return m_version == 1 && m_action == ContractAction::REMOVE;

        case ContractType::POLL:     return m_action == ContractAction::REMOVE;
        case ContractType::PROJECT:  return true;
        case ContractType::PROTOCOL: return true;
        case ContractType::SCRAPER:  return true;
        case ContractType::VOTE:     return m_action == ContractAction::REMOVE;
        default:                     return false;
    }
}

bool Contract::RequiresMessageKey() const
{
    switch (m_type.Value()) {
        case ContractType::BEACON: return m_action == ContractAction::ADD;
        case ContractType::POLL:   return m_action == ContractAction::ADD;
        case ContractType::VOTE:   return m_action == ContractAction::ADD;
        default:                   return false;
    }
}

bool Contract::RequiresSpecialKey() const
{
    return RequiresMessageKey() || RequiresMasterKey();
}

const CPubKey& Contract::ResolvePublicKey() const
{
    if (RequiresMessageKey()) {
        return MessagePublicKey();
    }

    if (RequiresMasterKey()) {
        return CWallet::MasterPublicKey();
    }

    return m_public_key.Key();
}

int64_t Contract::RequiredBurnAmount() const
{
    return m_body.m_payload->RequiredBurnAmount();
}

bool Contract::WellFormed() const
{
    return m_version > 0 && m_version <= Contract::CURRENT_VERSION
        && m_type != ContractType::UNKNOWN
        && m_action != ContractAction::UNKNOWN
        && m_body.WellFormed(m_action.Value())
        // Version 2+ contracts rely on the signatures in the transactions
        // instead of embedding another signature in the contract:
        && (m_version > 1 || m_signature.Viable())
        && (m_version > 1 || (RequiresSpecialKey() || m_public_key.Viable()));
}

bool Contract::Validate() const
{
    return WellFormed()
        // Version 2+ contracts rely on the signatures in the transactions
        // instead of embedding another signature in the contract:
        && (m_version > 1 || VerifySignature());
}

ContractPayload Contract::SharePayload() const
{
    if (m_version > 1) {
        return m_body.m_payload;
    }

    return m_body.ConvertFromLegacy(m_type.Value());
}

bool Contract::Sign(CKey& private_key)
{
    std::vector<unsigned char> output;

    if (!private_key.Sign(GetHash(), output)) {
        Log("ERROR: Failed to sign contract");
        return false;
    }

    m_signature = std::move(output);

    if (!RequiresSpecialKey()) {
        m_public_key = private_key.GetPubKey();
    }

    return true;
}

bool Contract::SignWithMessageKey()
{
    CKey key;

    key.SetPrivKey(MessagePrivateKey());

    return Sign(key);
}

bool Contract::VerifySignature() const
{
    CKey key;

    if (!key.SetPubKey(ResolvePublicKey())) {
        Log("ERROR: Failed to set contract public key");
        return false;
    }

    return key.Verify(GetHash(), m_signature.Raw());
}

uint256 Contract::GetHash() const
{
    if (m_version > 1) {
        return SerializeHash(*this);
    }

    const std::string type_string = m_type.ToString();

    // We use static_cast here instead of dynamic_cast to avoid the lookup. The
    // value of m_payload is guaranteed to be a LegacyPayload for v1 contracts.
    //
    const auto& payload = static_cast<const LegacyPayload&>(*m_body.m_payload);

    return Hash(
        type_string.begin(),
        type_string.end(),
        payload.m_key.begin(),
        payload.m_key.end(),
        payload.m_value.begin(),
        payload.m_value.end());
}

std::string Contract::ToString() const
{
    if (m_type == ContractType::MESSAGE) {
        return "<MESSAGE>" + m_body.m_payload->LegacyValueString() + "</MESSAGE>";
    }

    return "<MT>" + m_type.ToString()                     + "</MT>"
        + "<MK>"  + m_body.m_payload->LegacyKeyString()   + "</MK>"
        + "<MV>"  + m_body.m_payload->LegacyValueString() + "</MV>"
        + "<MA>"  + m_action.ToString()                   + "</MA>"
        + "<MPK>" + m_public_key.ToString()               + "</MPK>"
        + "<MS>"  + m_signature.ToString()                + "</MS>";
}

void Contract::Log(const std::string& prefix) const
{
    // TODO: temporary... needs better logging
    LogPrint(BCLog::LogFlags::CONTRACT,
        "<Contract::Log>: %s: v%d, %s, %s, %s, %s, %s, %s",
        prefix,
        m_version,
        m_type.ToString(),
        m_action.ToString(),
        m_body.m_payload->LegacyKeyString(),
        m_body.m_payload->LegacyValueString(),
        m_public_key.ToString(),
        m_signature.ToString());
}

// -----------------------------------------------------------------------------
// Class: Contract::Type
// -----------------------------------------------------------------------------

Contract::Type::Type(ContractType type) : EnumByte(type)
{
}

Contract::Type Contract::Type::Parse(std::string input)
{
    // Ordered by frequency:
    if (input == "beacon")         return ContractType::BEACON;
    if (input == "vote")           return ContractType::VOTE;
    if (input == "poll")           return ContractType::POLL;
    if (input == "project")        return ContractType::PROJECT;
    if (input == "scraper")        return ContractType::SCRAPER;
    if (input == "protocol")       return ContractType::PROTOCOL;

    return ContractType::UNKNOWN;
}

std::string Contract::Type::ToString() const
{
    switch (m_value) {
        case ContractType::BEACON:     return "beacon";
        case ContractType::CLAIM:      return "claim";
        case ContractType::MESSAGE:    return "message";
        case ContractType::POLL:       return "poll";
        case ContractType::PROJECT:    return "project";
        case ContractType::PROTOCOL:   return "protocol";
        case ContractType::SCRAPER:    return "scraper";
        case ContractType::VOTE:       return "vote";
        default:                       return "";
    }
}

// -----------------------------------------------------------------------------
// Class: Contract::Action
// -----------------------------------------------------------------------------

Contract::Action::Action(ContractAction action) : EnumByte(action)
{
}

Contract::Action Contract::Action::Parse(std::string input)
{
    if (input == "A")  return ContractAction::ADD;
    if (input == "D")  return ContractAction::REMOVE;

    return ContractAction::UNKNOWN;
}

std::string Contract::Action::ToString() const
{
    switch (m_value) {
        case ContractAction::ADD:    return "A";
        case ContractAction::REMOVE: return "D";
        default:                     return "";
    }
}

// -----------------------------------------------------------------------------
// Class: Contract::Body
// -----------------------------------------------------------------------------

Contract::Body::Body()
    : m_payload(ContractPayload::Make<EmptyPayload>())
{
}

Contract::Body::Body(ContractPayload payload)
    : m_payload(std::move(payload))
{
}

bool Contract::Body::WellFormed(const ContractAction action) const
{
    return m_payload->WellFormed(action);
}

ContractPayload Contract::Body::AssumeLegacy() const
{
    return m_payload;
}

ContractPayload Contract::Body::ConvertFromLegacy(const ContractType type) const
{
    // We use static_cast here instead of dynamic_cast to avoid the lookup. The
    // value of m_payload is guaranteed to be a LegacyPayload for v1 contracts.
    //
    const auto& legacy = static_cast<const LegacyPayload&>(*m_payload);

    switch (type) {
        case ContractType::UNKNOWN:
            return ContractPayload::Make<EmptyPayload>();
        case ContractType::BEACON:
            return ContractPayload::Make<BeaconPayload>(
                BeaconPayload::Parse(legacy.m_key, legacy.m_value));
        case ContractType::CLAIM:
            // Claims can only exist in a coinbase transaction and have no
            // legacy representation as a contract:
            assert(false && "Attempted to convert legacy claim contract.");
        case ContractType::MESSAGE:
            // The contract system does not map legacy transaction messages
            // stored in the CTransaction::hashBoinc field:
            assert(false && "Attempted to convert legacy message contract.");
        case ContractType::POLL:
            return ContractPayload::Make<PollPayload>(Poll::Parse(legacy.m_value));
        case ContractType::PROJECT:
            return ContractPayload::Make<Project>(legacy.m_key, legacy.m_value, 0);
        case ContractType::PROTOCOL:
            return m_payload;
        case ContractType::SCRAPER:
            return m_payload;
        case ContractType::VOTE:
            return ContractPayload::Make<LegacyVote>(
                LegacyVote::Parse(legacy.m_key, legacy.m_value));
        case ContractType::OUT_OF_BOUND:
            assert(false);
    }

    return ContractPayload::Make<EmptyPayload>();
}

void Contract::Body::ResetType(const ContractType type)
{
    switch (type) {
        case ContractType::UNKNOWN:
            m_payload.Reset(new EmptyPayload());
            break;
        case ContractType::BEACON:
            m_payload.Reset(new BeaconPayload());
            break;
        case ContractType::CLAIM:
            m_payload.Reset(new Claim());
            break;
        case ContractType::MESSAGE:
            m_payload.Reset(new TxMessage());
            break;
        case ContractType::POLL:
            m_payload.Reset(new PollPayload());
            break;
        case ContractType::PROJECT:
            m_payload.Reset(new Project());
            break;
        case ContractType::PROTOCOL:
            m_payload.Reset(new LegacyPayload());
            break;
        case ContractType::SCRAPER:
            m_payload.Reset(new LegacyPayload());
            break;
        case ContractType::VOTE:
            m_payload.Reset(new Vote());
            break;
        case ContractType::OUT_OF_BOUND:
            assert(false);
    }
}

// -----------------------------------------------------------------------------
// Class: Contract::Signature
// -----------------------------------------------------------------------------

Contract::Signature::Signature() : m_bytes()
{
}

Contract::Signature::Signature(std::vector<unsigned char> bytes)
    : m_bytes(std::move(bytes))
{
}

Contract::Signature Contract::Signature::Parse(const std::string& input)
{
    if (input.empty()) {
        return Contract::Signature();
    }

    bool invalid;
    std::vector<unsigned char> decoded = DecodeBase64(input.c_str(), &invalid);

    if (invalid) {
        return Contract::Signature();
    }

    return Contract::Signature(std::move(decoded));
}

bool Contract::Signature::Viable() const
{
    // The DER-encoded ASN.1 ECDSA signatures typically contain 70 or 71 bytes,
    // but may hold up to 73. Sizes as low as 68 bytes seen on mainnet. We only
    // check the number of bytes here as an early step:
    return m_bytes.size() >= 64 && m_bytes.size() <= 73;
}

const std::vector<unsigned char>& Contract::Signature::Raw() const
{
    return m_bytes;
}

Contract Contract::ToLegacy() const
{
    return Contract(
        1,
        m_type,
        m_action,
        ContractPayload::Make<LegacyPayload>(
            m_body.m_payload->LegacyKeyString(),
            m_body.m_payload->LegacyValueString()),
        m_signature,
        m_public_key);
}

std::string Contract::Signature::ToString() const
{
    if (m_bytes.empty()) {
        return std::string();
    }

    return EncodeBase64(m_bytes.data(), m_bytes.size());
}

// -----------------------------------------------------------------------------
// Class: Contract::PublicKey
// -----------------------------------------------------------------------------

Contract::PublicKey::PublicKey() : m_key()
{
}

Contract::PublicKey::PublicKey(CPubKey key)
    : m_key(std::move(key))
{
}

Contract::PublicKey Contract::PublicKey::Parse(const std::string& input)
{
    if (input.empty()) {
        return Contract::PublicKey();
    }

    return Contract::PublicKey(CPubKey(ParseHex(input)));
}

bool Contract::PublicKey::operator==(const CPubKey& other) const
{
    return m_key == other;
}

bool Contract::PublicKey::operator!=(const CPubKey& other) const
{
    return m_key != other;
}

bool Contract::PublicKey::Viable() const
{
    return m_key.IsValid();
}

const CPubKey& Contract::PublicKey::Key() const
{
    return m_key;
}

std::string Contract::PublicKey::ToString() const
{
    return HexStr(m_key.Raw());
}

// -----------------------------------------------------------------------------
// Abstract Class: IContractHandler
// -----------------------------------------------------------------------------

void IContractHandler::Revert(const ContractContext& ctx)
{
    if (ctx->m_action == ContractAction::ADD) {
        Delete(ctx);
        return;
    }

    if (ctx->m_action == ContractAction::REMOVE) {
        Add(ctx);
        return;
    }

    error("Unknown contract action ignored: %s", ctx->m_action.ToString());
}
