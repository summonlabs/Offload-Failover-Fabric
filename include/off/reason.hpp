// Offload Failover Fabric - stable machine-readable reason codes.
// Codes are append-only: an existing code never changes meaning or numeric
// value. Semantics are never carried by free-form strings.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string_view>

#include "off/strong.hpp"

namespace off {

// X(Identifier, "STABLE_CODE", value)
#define OFF_REASON_TABLE(X)                                                                       \
  /* -- success and positive rationale -------------------------------------- */                   \
  X(Ok, "OK", 0x0000)                                                                             \
  X(AcceptQualifiedPrimaryFailure, "ACCEPT_QUALIFIED_PRIMARY_FAILURE", 0x0101)                    \
  X(AcceptQualifiedReplicaFailure, "ACCEPT_QUALIFIED_REPLICA_FAILURE", 0x0102)                    \
  X(AcceptQualifiedPlannedRecovery, "ACCEPT_QUALIFIED_PLANNED_RECOVERY", 0x0103)                  \
  X(AcceptFallbackCapabilityCompatible, "ACCEPT_FALLBACK_CAPABILITY_COMPATIBLE", 0x0110)          \
  X(AcceptFallbackHealthy, "ACCEPT_FALLBACK_HEALTHY", 0x0111)                                     \
  X(AcceptFenceAdvanced, "ACCEPT_FENCE_ADVANCED", 0x0120)                                         \
  X(AcceptLeaseTransferred, "ACCEPT_LEASE_TRANSFERRED", 0x0121)                                   \
  X(AcceptIntentEmitted, "ACCEPT_INTENT_EMITTED", 0x0130)                                         \
  X(AcceptEffectVerified, "ACCEPT_EFFECT_VERIFIED", 0x0140)                                       \
  X(AcceptContinuityVerified, "ACCEPT_CONTINUITY_VERIFIED", 0x0141)                               \
  X(AcceptContinuityDegradedStateful, "ACCEPT_CONTINUITY_DEGRADED_STATEFUL", 0x0142)              \
  X(AcceptDuplicateIdempotent, "ACCEPT_DUPLICATE_IDEMPOTENT", 0x0150)                             \
  X(AcceptDuplicateReplayCached, "ACCEPT_DUPLICATE_REPLAY_CACHED", 0x0151)                        \
  X(AcceptRecoveryReissueSameFence, "ACCEPT_RECOVERY_REISSUE_SAME_FENCE", 0x0152)                 \
  X(AcceptAmbiguityResolvedFailover, "ACCEPT_AMBIGUITY_RESOLVED_FAILOVER", 0x0160)                \
  X(AcceptAmbiguityResolvedNoFailover, "ACCEPT_AMBIGUITY_RESOLVED_NO_FAILOVER", 0x0161)           \
  X(AcceptFailbackGenerationFresh, "ACCEPT_FAILBACK_GENERATION_FRESH", 0x0170)                    \
  X(AcceptTargetRegistered, "ACCEPT_TARGET_REGISTERED", 0x0180)                                   \
  X(AcceptTopologyGenerationAdvanced, "ACCEPT_TOPOLOGY_GENERATION_ADVANCED", 0x0181)              \
  X(AcceptPolicyGenerationAdvanced, "ACCEPT_POLICY_GENERATION_ADVANCED", 0x0182)                  \
  X(AcceptStateTransferVerified, "ACCEPT_STATE_TRANSFER_VERIFIED", 0x0190)                        \
  X(AcceptPlacementEstablished, "ACCEPT_PLACEMENT_ESTABLISHED", 0x0183)                           \
  X(AcceptSourceEnrolled, "ACCEPT_SOURCE_ENROLLED", 0x0184)                                       \
  X(AcceptTargetRetired, "ACCEPT_TARGET_RETIRED", 0x0185)                                         \
  X(AcceptServiceRegistered, "ACCEPT_SERVICE_REGISTERED", 0x0186)                                 \
  X(AcceptCapabilityRecorded, "ACCEPT_CAPABILITY_RECORDED", 0x0187)                               \
  X(AcceptFailureRecorded, "ACCEPT_FAILURE_RECORDED", 0x0188)                                     \
  X(AcceptDependencyRecorded, "ACCEPT_DEPENDENCY_RECORDED", 0x0189)                               \
  X(AcceptServiceWithdrawn, "ACCEPT_SERVICE_WITHDRAWN", 0x018A)                                   \
  \
  /* -- shape, encoding and framing ----------------------------------------- */                   \
  X(Malformed, "MALFORMED", 0x0201)                                                               \
  X(Truncated, "TRUNCATED", 0x0202)                                                               \
  X(Oversized, "OVERSIZED", 0x0203)                                                               \
  X(Corrupt, "CORRUPT", 0x0204)                                                                   \
  X(ChecksumMismatch, "CHECKSUM_MISMATCH", 0x0205)                                                \
  X(UnsupportedVersion, "UNSUPPORTED_VERSION", 0x0206)                                            \
  X(UnsupportedSemantics, "UNSUPPORTED_SEMANTICS", 0x0207)                                        \
  X(UnknownField, "UNKNOWN_FIELD", 0x0208)                                                        \
  X(ImpossibleValue, "IMPOSSIBLE_VALUE", 0x0209)                                                  \
  X(DepthExceeded, "DEPTH_EXCEEDED", 0x020A)                                                      \
  X(BudgetExceeded, "BUDGET_EXCEEDED", 0x020B)                                                    \
  \
  /* -- identity and registry ------------------------------------------------ */                  \
  X(UnknownService, "UNKNOWN_SERVICE", 0x0301)                                                    \
  X(UnknownScope, "UNKNOWN_SCOPE", 0x0302)                                                        \
  X(UnknownTarget, "UNKNOWN_TARGET", 0x0303)                                                      \
  X(UnknownSource, "UNKNOWN_SOURCE", 0x0304)                                                      \
  X(UnknownDevice, "UNKNOWN_DEVICE", 0x0305)                                                      \
  X(UnknownIncarnation, "UNKNOWN_INCARNATION", 0x0306)                                            \
  X(DuplicateService, "DUPLICATE_SERVICE", 0x0307)                                                \
  X(DuplicateTarget, "DUPLICATE_TARGET", 0x0308)                                                  \
  X(DuplicateIdentity, "DUPLICATE_IDENTITY", 0x0309)                                              \
  X(RegistryCapacityExceeded, "REGISTRY_CAPACITY_EXCEEDED", 0x030A)                               \
  \
  /* -- generations and compatibility ---------------------------------------- */                  \
  X(StaleGeneration, "STALE_GENERATION", 0x0401)                                                  \
  X(SupersededGeneration, "SUPERSEDED_GENERATION", 0x0402)                                        \
  X(FutureGeneration, "FUTURE_GENERATION", 0x0403)                                                \
  X(GenerationConflict, "GENERATION_CONFLICT", 0x0404)                                            \
  X(GenerationExhausted, "GENERATION_EXHAUSTED", 0x0405)                                          \
  X(IncompatibleCapability, "INCOMPATIBLE_CAPABILITY", 0x0406)                                    \
  X(IncompatibleSemantics, "INCOMPATIBLE_SEMANTICS", 0x0407)                                      \
  X(IncarnationMismatch, "INCARNATION_MISMATCH", 0x0408)                                          \
  X(IncarnationRegression, "INCARNATION_REGRESSION", 0x0409)                                      \
  \
  /* -- evidence -------------------------------------------------------------- */                  \
  X(EvidenceMissing, "EVIDENCE_MISSING", 0x0501)                                                  \
  X(EvidenceStale, "EVIDENCE_STALE", 0x0502)                                                      \
  X(EvidenceSuperseded, "EVIDENCE_SUPERSEDED", 0x0503)                                            \
  X(EvidenceAmbiguous, "EVIDENCE_AMBIGUOUS", 0x0504)                                              \
  X(EvidenceConflicting, "EVIDENCE_CONFLICTING", 0x0505)                                          \
  X(EvidenceIncomplete, "EVIDENCE_INCOMPLETE", 0x0506)                                            \
  X(EvidenceUnknownSource, "EVIDENCE_UNKNOWN_SOURCE", 0x0507)                                     \
  X(EvidenceUnsupportedKind, "EVIDENCE_UNSUPPORTED_KIND", 0x0508)                                 \
  X(EvidenceTargetMismatch, "EVIDENCE_TARGET_MISMATCH", 0x0509)                                   \
  X(EvidenceTopologyMismatch, "EVIDENCE_TOPOLOGY_MISMATCH", 0x050A)                               \
  X(EvidenceCapacityExceeded, "EVIDENCE_CAPACITY_EXCEEDED", 0x050B)                               \
  X(EvidenceReplayRejected, "EVIDENCE_REPLAY_REJECTED", 0x050C)                                   \
  X(EvidenceReporterUnauthorized, "EVIDENCE_REPORTER_UNAUTHORIZED", 0x050D)                       \
  \
  /* -- authority, epoch, lease ------------------------------------------------ */                 \
  X(NoAuthority, "NO_AUTHORITY", 0x0601)                                                          \
  X(AuthoritySuperseded, "AUTHORITY_SUPERSEDED", 0x0602)                                          \
  X(AuthorityExpired, "AUTHORITY_EXPIRED", 0x0603)                                                \
  X(EpochMismatch, "EPOCH_MISMATCH", 0x0604)                                                      \
  X(BootMismatch, "BOOT_MISMATCH", 0x0605)                                                        \
  X(LeaseNotHeld, "LEASE_NOT_HELD", 0x0606)                                                       \
  X(LeaseTermStale, "LEASE_TERM_STALE", 0x0607)                                                   \
  X(LeaseTermExhausted, "LEASE_TERM_EXHAUSTED", 0x0608)                                           \
  X(CoordinatorNotPrimary, "COORDINATOR_NOT_PRIMARY", 0x0609)                                     \
  \
  /* -- durable fence ---------------------------------------------------------- */                 \
  X(FenceStale, "FENCE_STALE", 0x0701)                                                            \
  X(FenceRegression, "FENCE_REGRESSION", 0x0702)                                                  \
  X(FenceMismatch, "FENCE_MISMATCH", 0x0703)                                                      \
  X(FenceNotDurable, "FENCE_NOT_DURABLE", 0x0704)                                                 \
  X(FenceExhausted, "FENCE_EXHAUSTED", 0x0705)                                                    \
  X(FenceHeldByOther, "FENCE_HELD_BY_OTHER", 0x0706)                                              \
  \
  /* -- eligibility and fallback selection ------------------------------------- */                \
  X(TargetDisappearanceIsNotPermission, "TARGET_DISAPPEARANCE_IS_NOT_PERMISSION", 0x0801)         \
  X(NoEligibleFallback, "NO_ELIGIBLE_FALLBACK", 0x0802)                                           \
  X(FallbackUnsupported, "FALLBACK_UNSUPPORTED", 0x0803)                                          \
  X(FallbackIncompatible, "FALLBACK_INCOMPATIBLE", 0x0804)                                        \
  X(FallbackFenced, "FALLBACK_FENCED", 0x0805)                                                    \
  X(FallbackIsActiveTarget, "FALLBACK_IS_ACTIVE_TARGET", 0x0806)                                  \
  X(FallbackUnhealthy, "FALLBACK_UNHEALTHY", 0x0807)                                              \
  X(FallbackDependencyUnmet, "FALLBACK_DEPENDENCY_UNMET", 0x0808)                                 \
  X(FallbackCapabilityGenerationStale, "FALLBACK_CAPABILITY_GENERATION_STALE", 0x0809)            \
  X(FallbackNotInGovernedScope, "FALLBACK_NOT_IN_GOVERNED_SCOPE", 0x080A)                         \
  X(FallbackCandidateLimitReached, "FALLBACK_CANDIDATE_LIMIT_REACHED", 0x080B)                    \
  \
  /* -- dependency graph -------------------------------------------------------- */                \
  X(DependencyUnknown, "DEPENDENCY_UNKNOWN", 0x0901)                                              \
  X(DependencyUnresolved, "DEPENDENCY_UNRESOLVED", 0x0902)                                        \
  X(DependencyCycle, "DEPENDENCY_CYCLE", 0x0903)                                                  \
  X(DependencyHardUnmet, "DEPENDENCY_HARD_UNMET", 0x0904)                                         \
  X(DependencyDepthExceeded, "DEPENDENCY_DEPTH_EXCEEDED", 0x0905)                                 \
  X(DependencyCapacityExceeded, "DEPENDENCY_CAPACITY_EXCEEDED", 0x0906)                           \
  X(DependencySelfReference, "DEPENDENCY_SELF_REFERENCE", 0x0907)                                 \
  \
  /* -- lifecycle, state and budgets -------------------------------------------- */                \
  X(InvalidState, "INVALID_STATE", 0x0A01)                                                        \
  X(TransitionRefused, "TRANSITION_REFUSED", 0x0A02)                                              \
  X(FailoverAlreadyInProgress, "FAILOVER_ALREADY_IN_PROGRESS", 0x0A03)                           \
  X(NoFailoverInProgress, "NO_FAILOVER_IN_PROGRESS", 0x0A04)                                      \
  X(AttemptBudgetExhausted, "ATTEMPT_BUDGET_EXHAUSTED", 0x0A05)                                   \
  X(PlanStepLimitReached, "PLAN_STEP_LIMIT_REACHED", 0x0A06)                                      \
  X(HistoryTruncated, "HISTORY_TRUNCATED", 0x0A07)                                                \
  X(QueueCapacityExceeded, "QUEUE_CAPACITY_EXCEEDED", 0x0A08)                                     \
  X(ResourceExhausted, "RESOURCE_EXHAUSTED", 0x0A09)                                              \
  X(PayloadTooLarge, "PAYLOAD_TOO_LARGE", 0x0A0A)                                                 \
  X(ServiceNotRegistered, "SERVICE_NOT_REGISTERED", 0x0A0B)                                       \
  X(ServiceStateIncompatible, "SERVICE_STATE_INCOMPATIBLE", 0x0A0C)                               \
  \
  /* -- effect and continuity ---------------------------------------------------- */               \
  X(EffectMissing, "EFFECT_MISSING", 0x0B01)                                                      \
  X(EffectUnverified, "EFFECT_UNVERIFIED", 0x0B02)                                                \
  X(EffectMismatch, "EFFECT_MISMATCH", 0x0B03)                                                    \
  X(EffectStale, "EFFECT_STALE", 0x0B04)                                                          \
  X(EffectReporterUnauthorized, "EFFECT_REPORTER_UNAUTHORIZED", 0x0B05)                           \
  X(EffectNegative, "EFFECT_NEGATIVE", 0x0B06)                                                    \
  X(ContinuityNotVerified, "CONTINUITY_NOT_VERIFIED", 0x0B07)                                     \
  X(EffectAlreadyVerified, "EFFECT_ALREADY_VERIFIED", 0x0B08)                                     \
  X(EffectAttemptMismatch, "EFFECT_ATTEMPT_MISMATCH", 0x0B09)                                     \
  X(EffectUnsupportedKind, "EFFECT_UNSUPPORTED_KIND", 0x0B0A)                                     \
  \
  /* -- ambiguity ------------------------------------------------------------------ */              \
  X(AmbiguityUnresolved, "AMBIGUITY_UNRESOLVED", 0x0C01)                                          \
  X(AmbiguityAlreadyResolved, "AMBIGUITY_ALREADY_RESOLVED", 0x0C02)                               \
  X(AmbiguityDualActiveRisk, "AMBIGUITY_DUAL_ACTIVE_RISK", 0x0C03)                                \
  X(AmbiguityRequiresOperator, "AMBIGUITY_REQUIRES_OPERATOR", 0x0C04)                             \
  \
  /* -- statefulness ---------------------------------------------------------------- */            \
  X(StateTransferMissing, "STATE_TRANSFER_MISSING", 0x0D01)                                       \
  X(StateGenerationStale, "STATE_GENERATION_STALE", 0x0D02)                                       \
  X(StateTransferIncompatible, "STATE_TRANSFER_INCOMPATIBLE", 0x0D03)                             \
  X(StatefulRecoveryRequiresTransfer, "STATEFUL_RECOVERY_REQUIRES_TRANSFER", 0x0D04)              \
  \
  /* -- failback --------------------------------------------------------------------- */            \
  X(FailbackNotEligible, "FAILBACK_NOT_ELIGIBLE", 0x0E01)                                         \
  X(FailbackStaleGeneration, "FAILBACK_STALE_GENERATION", 0x0E02)                                 \
  X(FailbackOriginalUnavailable, "FAILBACK_ORIGINAL_UNAVAILABLE", 0x0E03)                         \
  X(FailbackOriginalFenceStale, "FAILBACK_ORIGINAL_FENCE_STALE", 0x0E04)                          \
  X(FailbackNoFailoverRecorded, "FAILBACK_NO_FAILOVER_RECORDED", 0x0E05)                          \
  X(FailbackInProgress, "FAILBACK_IN_PROGRESS", 0x0E06)                                           \
  \
  /* -- persistence and restart ------------------------------------------------------ */           \
  X(StoreUnavailable, "STORE_UNAVAILABLE", 0x0F01)                                                \
  X(StoreCorrupt, "STORE_CORRUPT", 0x0F02)                                                        \
  X(StoreTruncated, "STORE_TRUNCATED", 0x0F03)                                                    \
  X(StoreVersionIncompatible, "STORE_VERSION_INCOMPATIBLE", 0x0F04)                              \
  X(StoreSemanticsIncompatible, "STORE_SEMANTICS_INCOMPATIBLE", 0x0F05)                          \
  X(SnapshotMissing, "SNAPSHOT_MISSING", 0x0F06)                                                  \
  X(SnapshotCorrupt, "SNAPSHOT_CORRUPT", 0x0F07)                                                  \
  X(TornTailTruncated, "TORN_TAIL_TRUNCATED", 0x0F08)                                             \
  X(IncompleteTransactionDropped, "INCOMPLETE_TRANSACTION_DROPPED", 0x0F09)                       \
  X(RestartConservativeDowngrade, "RESTART_CONSERVATIVE_DOWNGRADE", 0x0F0A)                      \
  X(RestartEpochAdvanced, "RESTART_EPOCH_ADVANCED", 0x0F0B)                                      \
  X(RestartIncarnationAdvanced, "RESTART_INCARNATION_ADVANCED", 0x0F0C)                          \
  X(JournalRotated, "JOURNAL_ROTATED", 0x0F0D)                                                    \
  X(JournalCapacityExceeded, "JOURNAL_CAPACITY_EXCEEDED", 0x0F0E)                                 \
  X(PersistenceWriteFailed, "PERSISTENCE_WRITE_FAILED", 0x0F0F)                                   \
  \
  /* -- cancellation and shutdown ---------------------------------------------------- */           \
  X(Cancelled, "CANCELLED", 0x1001)                                                               \
  X(CancelTooLate, "CANCEL_TOO_LATE", 0x1002)                                                     \
  X(ShuttingDown, "SHUTTING_DOWN", 0x1003)                                                        \
  X(NotShuttingDown, "NOT_SHUTTING_DOWN", 0x1004)                                                 \
  \
  /* -- transport and protocol ------------------------------------------------------- */           \
  X(ProtocolViolation, "PROTOCOL_VIOLATION", 0x1101)                                              \
  X(ProtocolVersionMismatch, "PROTOCOL_VERSION_MISMATCH", 0x1102)                                 \
  X(FrameTooLarge, "FRAME_TOO_LARGE", 0x1103)                                                     \
  X(RequestUnknown, "REQUEST_UNKNOWN", 0x1104)                                                    \
  X(NotHandshaken, "NOT_HANDSHAKEN", 0x1105)                                                      \
  X(ConnectionLimitReached, "CONNECTION_LIMIT_REACHED", 0x1106)                                   \
  X(TransportClosed, "TRANSPORT_CLOSED", 0x1107)                                                  \
  X(RequestKeyConflict, "REQUEST_KEY_CONFLICT", 0x1108)                                           \
  \
  /* -- internal ---------------------------------------------------------------------- */          \
  X(Internal, "INTERNAL", 0x1201)                                                                 \
  X(NotImplemented, "NOT_IMPLEMENTED", 0x1202)                                                    \
  X(InvariantViolated, "INVARIANT_VIOLATED", 0x1203)

enum class Reason : u16 {
#define OFF_REASON_ENUM(identifier, text, value) identifier = value,
  OFF_REASON_TABLE(OFF_REASON_ENUM)
#undef OFF_REASON_ENUM
};

/// Stable machine-readable code text. Never localized, never reformatted.
[[nodiscard]] std::string_view reason_code_text(Reason reason) noexcept;

/// True when the code denotes an accepted decision (class 0x00 or 0x01).
[[nodiscard]] bool reason_is_accept(Reason reason) noexcept;

/// Stable textual category of the code family.
[[nodiscard]] std::string_view reason_family(Reason reason) noexcept;

/// Human-readable one-line summary for inspection tooling. Derived
/// deterministically from the stable code text; never localized.
[[nodiscard]] std::string reason_summary(Reason reason);

/// Parse a stable code text back into a code. Returns nullopt for unknown text
/// instead of guessing.
[[nodiscard]] bool reason_from_text(std::string_view text, Reason& out) noexcept;

}  // namespace off
