# Proof obligations

Each obligation is discharged by an executable test. Test names below are the
`suite.case` identifiers printed by `off_tests` and `off_process_tests`.

## 1. Old targets cannot resume behind a higher durable fence

* Discharged by: `fence.an_old_target_cannot_resume_behind_a_higher_token`,
  `fence.a_fenced_target_stays_out_until_its_incarnation_advances`,
  `fence.a_new_incarnation_releases_the_fence`,
  `fence.retired_target_never_becomes_a_candidate`,
  `process.daemon_serves_a_full_recovery_over_real_sockets`.
* Mechanism: every recovery mints a strictly higher `FenceToken` and records a
  witness `(token, incarnation)` for the target it left. `verify_fence`
  refuses a presented token below the current one, above the current one, from
  the wrong holder, and from a holder whose incarnation is not newer than the
  witness.

## 2. Failover requires supported, compatible fallback

* Discharged by: `qualify.fallback_must_support_the_required_capabilities`,
  `qualify.missing_capability_evidence_is_not_absence_of_requirement`,
  `qualify.candidate_outside_the_governed_scope_is_never_used`,
  `qualify.no_candidate_at_all_is_reported_as_such`,
  `qualify.operator_authorization_does_not_replace_capability_support`.
* Mechanism: candidate evaluation requires current capability evidence whose
  incarnation and topology generation match, a capability superset of the
  service requirement, and an unfenced incarnation. Operator authorization is
  checked separately and never substitutes for capability support.

## 3. Unknown dependencies block unsafe recovery

* Discharged by: `qualify.a_hard_dependency_without_evidence_blocks_placement`,
  `qualify.an_unknown_dependency_health_blocks_recovery`,
  `qualify.a_dependency_that_fails_later_blocks_recovery`,
  `qualify.an_observation_for_an_unregistered_dependency_is_refused`,
  `restart.dependency_observations_require_refresh`.
* Mechanism: a missing observation, an observation flagged for re-confirmation,
  an observation from a previous epoch or boot, an observation bound to a
  different topology generation, and an explicit `Unknown` health are all
  distinct from healthy and all block a hard dependency. Missing evidence never
  becomes healthy.

## 4. Duplicate and replayed attempts are idempotent or fenced

* Discharged by: `concurrency.duplicate_concurrent_requests_commit_exactly_once`,
  `process.concurrent_clients_cannot_double_commit`,
  `qualify.replayed_and_conflicting_evidence_is_refused`,
  `effects.duplicate_effect_identifiers_are_idempotent`,
  `effects.intent_acknowledgement_is_fenced_against_the_current_attempt`.
* Mechanism: a request key that has already been decided returns the recorded
  decision with `duplicate = true`; a key still in flight is refused; a
  per-source watermark rejects an older evidence sequence and reports a reused
  sequence with different content as a conflict; a repeated effect identifier is
  recognised as a replay.

## 5. Crash after commit and before acknowledgement stays conservative

* Discharged by: `restart.in_flight_recovery_is_downgraded_and_never_resurrected`,
  `restart.clean_reopen_preserves_placements_fences_and_history`,
  `store.incomplete_tail_transaction_is_dropped_and_counted`.
* Mechanism: an attempt is durably committed before the caller sees it. On
  restart, an attempt that was committed but never acknowledged is classified
  `OutcomeUnknown` and marked `restart_downgraded`; the recovery phase is
  pushed back to `IntentRecorded`, the lifecycle to `RecoveryPending`, and
  continuity to `Unknown`. A resume re-issues the *same* attempt at the *same*
  fence and generation, so an executor that already saw it treats the
  re-delivery as a duplicate rather than as new authority.

## 6. Failback cannot reuse stale generations

* Discharged by: `failback.stale_observed_generations_are_refused`,
  `failback.the_original_target_must_be_re_adopted_at_a_newer_incarnation`,
  `failback.a_superseded_capability_generation_cannot_be_reused`,
  `failback.fresh_generations_produce_a_governed_reverse_recovery`,
  `failback.a_second_failback_without_a_new_failover_is_refused`.
* Mechanism: failback requires the caller's observed topology and policy
  generations to be current, the original target to be re-adopted at a strictly
  newer incarnation, and a capability generation strictly newer than the one the
  failover already superseded.

## 7. Continuity is not claimed until verified effect exists

* Discharged by: `stateful.continuity_is_unknown_until_an_effect_is_verified`,
  `stateful.a_healthy_report_without_an_accepted_activation_is_not_verified`,
  `stateful.stateful_recovery_is_degraded_without_a_verified_transfer`,
  `stateful.verified_transfer_promotes_continuity_to_full`,
  `stateful.degraded_continuity_can_be_forbidden_by_policy`,
  `restart.verified_effect_does_not_survive_as_claimed_continuity`,
  `property.randomised_operation_sequences_preserve_invariants`.
* Mechanism: continuity starts `Unknown`; every placement or recovery resets it
  through `invalidate_verification`; only a positive `ServiceHealthy` effect
  after an accepted activation can raise it, and a stateful service without a
  verified state transfer reaches `Degraded` at most.

## 8. Accepted state is deterministic from accepted evidence and policy

* Discharged by: `concurrency.ingest_order_does_not_change_the_semantic_result`,
  `concurrency.concurrent_ingest_matches_the_serial_result`,
  `property.digests_and_exports_are_stable_across_repetition`,
  `planner.candidate_truncation_is_observable_and_order_independent`.
* Mechanism: the semantic projection excludes delivery-order artefacts, candidate
  ordering is total and canonical, and the candidate ceiling is applied after
  ordering so truncation cannot change which candidate wins.

## 9. Stale or superseded evidence cannot justify current authority

* Discharged by: `qualify.stale_evidence_cannot_justify_recovery`,
  `restart.evidence_from_a_previous_epoch_cannot_justify_new_authority`,
  `qualify.topology_generation_change_invalidates_evidence_bindings`,
  `adversarial.future_generations_are_refused`,
  `effects.epoch_and_boot_mismatches_are_refused`.
* Mechanism: evidence is bound to the coordinator epoch, boot, topology
  generation, capability generation and policy generation under which it was
  accepted, and is additionally aged against a policy freshness bound in
  coordinator ticks.

## 10. Missing evidence never silently becomes false, zero or success

* Discharged by: `qualify.recovery_without_evidence_is_refused`,
  `qualify.disappeared_target_alone_is_never_permission`,
  `canonical.absence_is_distinct_from_zero_and_empty`,
  `model.empty_optional_names_round_trip_as_absence`,
  `adversarial.zero_sequence_and_zero_incarnation_evidence_are_refused`.
* Mechanism: optional values carry an explicit presence byte; a nil identity is
  never a real identity; an absent observation is `DependencyUnknown`, not
  healthy; a request with no evidence is refused rather than treated as "not
  failed".

## 11. Every bounded truncation, refusal and eviction is observable

* Discharged by: `planner.plan_ceiling_refuses_instead_of_silently_shortening`,
  `planner.candidate_truncation_is_observable_and_order_independent`,
  `property.explanations_are_bounded_and_eviction_is_accounted`,
  `store.incomplete_tail_transaction_is_dropped_and_counted`,
  `store.torn_tail_is_repaired_without_losing_committed_records`.
* Mechanism: a truncated plan is refused and reports the number of omitted steps;
  candidate truncation and dependency depth truncation increment counters;
  explanation and attempt eviction are counted; a dropped transaction and a
  repaired tail are recorded in the recovery report.

## 12. Persistence round-trips correctness-critical state without semantic loss

* Discharged by: `restart.clean_reopen_preserves_placements_fences_and_history`,
  `restart.snapshot_compaction_and_replay_round_trip_every_field`,
  `store.committed_transactions_replay_in_order`,
  `store.snapshot_compaction_round_trips_and_bounds_generations`,
  `model.every_persisted_entity_round_trips`.
* Mechanism: the snapshot encodes every persisted entity with its generation,
  incarnation, provenance and flags; the journal replays complete transactions
  only; the schema version is validated on read.

## 13. Conservative restart does not resurrect liveness or authority

* Discharged by: `restart.epoch_and_boot_advance_and_previous_authority_is_fenced`,
  `restart.capability_evidence_requires_reconfirmation`,
  `restart.dependency_observations_require_refresh`,
  `restart.verified_effect_does_not_survive_as_claimed_continuity`.
* Mechanism: restart advances the coordinator epoch and boot, marks all persisted
  capability evidence as requiring re-confirmation, marks all dependency
  observations as requiring refresh, invalidates failure evidence through the
  epoch check, and drops any verified effect rather than carrying a claim across
  the boundary.

## 14. Malformed, corrupt, truncated or oversized input cannot produce success

* Discharged by: `adversarial.frame_codec_rejects_every_malformed_shape`,
  `adversarial.oversized_frames_are_refused_before_allocation`,
  `adversarial.request_codec_rejects_trailing_and_unknown_bytes`,
  `adversarial.response_codec_rejects_unknown_reason_codes`,
  `adversarial.absurd_request_bodies_never_produce_a_valid_looking_success`,
  `adversarial.execution_of_unknown_request_kinds_fails_closed`,
  `property.canonical_decoding_never_accepts_random_bytes_as_a_typed_value`,
  `canonical.every_truncation_prefix_is_detected`,
  `store.corruption_before_a_valid_frame_fails_closed`,
  `store.future_and_unknown_format_versions_are_refused`,
  `store.corrupted_snapshot_fails_closed`,
  `restart.corrupt_journal_refuses_to_open_rather_than_guessing`.
* Mechanism: every decoder is bounds-checked against a declared ceiling before it
  allocates; a failed canonical writer clears its buffer so a truncated encoding
  can never be mistaken for a valid one; the store refuses to open rather than
  guessing when integrity cannot be established.

## 15. Cancellation and shutdown are real

* Discharged by: `concurrency.cancellation_never_publishes_success_after_being_recorded`,
  `concurrency.wait_for_intent_blocks_until_work_completes`,
  `concurrency.repeated_start_and_stop_returns_accounting_to_baseline`,
  `process.abrupt_client_disconnect_does_not_disturb_the_daemon`.
* Mechanism: the commit point holds the cancellation registry, so a cancellation
  either lands before the commit and aborts the operation, or is reported as
  `CancelTooLate`. Shutdown closes the listening socket and interrupts every
  live connection so no worker can outlive the join.

## 16. Resource and accounting closure

* Discharged by: `property.randomised_operation_sequences_preserve_invariants`,
  `property.explanations_are_bounded_and_eviction_is_accounted`,
  `concurrency.repeated_start_and_stop_returns_accounting_to_baseline`.
* Mechanism: every bounded structure is checked against its ceiling after each
  randomised step; repeated open/close returns the intent queue to empty and the
  accounting counters to a consistent baseline.
