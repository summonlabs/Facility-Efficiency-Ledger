#pragma once

// Concrete identity, generation, epoch, and revision types used across the
// ledger. Each is a distinct type so that, for example, a generation can never
// be passed where a revision is expected.

#include "fel/strong.hpp"

namespace fel {

struct LedgerIdTag;
struct SourceIdTag;
struct EntryIdTag;
struct AllocationIdTag;
struct ResidualIdTag;
struct CorrectionIdTag;
struct IntervalIdTag;
struct SealIdTag;
struct IdempotencyKeyTag;
struct AuthorityIdTag;
struct VoidIdTag;
struct EpochTag;
struct GenerationTag;
struct RevisionTag;
struct SequenceTag;
struct AttemptTag;

using LedgerId = Identifier<LedgerIdTag>;
using SourceId = Identifier<SourceIdTag>;
using EntryId = Identifier<EntryIdTag>;
using AllocationId = Identifier<AllocationIdTag>;
using ResidualId = Identifier<ResidualIdTag>;
using CorrectionId = Identifier<CorrectionIdTag>;
using IntervalId = Identifier<IntervalIdTag>;
using SealId = Identifier<SealIdTag>;
using IdempotencyKey = Identifier<IdempotencyKeyTag>;
using AuthorityId = Identifier<AuthorityIdTag>;
using VoidId = Identifier<VoidIdTag>;

using Generation = Counter<GenerationTag>;
using Epoch = Counter<EpochTag>;
using Revision = Counter<RevisionTag>;
using Sequence = Counter<SequenceTag>;
using Attempt = Counter<AttemptTag>;

// Cheap deterministic identifier minting for generated ids: <prefix>-<counter>
// rendered in zero padded hexadecimal. The counter is supplied by the caller so
// that minting never depends on hidden global state.
std::string mint_identifier_text(std::string_view prefix, std::uint64_t counter);

}  // namespace fel
