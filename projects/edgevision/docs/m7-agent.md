# M7 · Local visual task Agent (integration in progress)

The runtime Agent lets a user request camera work in natural language: take a
snapshot, detect fixed classes, ask about the image, inspect capture metrics or
save a report. The local model selects a registered tool; host code checks its
arguments and dependencies, then calls the shared ROS2/ControlRouter/TaskManager.
The repository's AGENTS.md and domain SKILL.md files are developer documentation.

This stage is **not accepted as complete**. Actual snapshot → same-frame detector
→ local VLM → detector restoration works on the board. Automatic planning still
selects incorrect/repeated tools for some requests. The fixed 22-task suite,
three required end-to-end traces and formal interference/resource gates remain.

## Actual implementation and evidence

| Layer | Entry and verified boundary |
|---|---|
| Shared task routing | TaskCommand/Input and ROS RunInspection; one logical task history, generation-aware identity, no second camera owner |
| Snapshot storage | SnapshotStore: default 16 records, 32MiB combined owned raw/PNG budget, 4MiB per image; same frame/generation reused for detector and VQA |
| Model process | VlmProcess: bounded JSONL, deadline/cancel, actual child reaping before detector reload; no CUDA address or capture lease crosses the process boundary |
| Model scheduling | Sole GPU worker destroys completed detector objects, releases its process CUDA context and trims unused allocator pages before VLM startup; cannot run against in-flight work |
| Python Agent | Registered tool schema, at most 6 tool steps, one active/one queued request, bounded reports/traces, actual 10s metric observation and failure receipts |
| Acceptance state | R2 native CTest 15/15 and three Humble packages; R1 seven legacy real-GPU lifecycle cases. These are distinct from complete Agent task acceptance |

[Checkpoint evidence](../evidence/2026-11-16-agent-checkpoint/manifest.json) retains
source snapshots, original failures, runtime receipts, prompt experiments and
resources. R2 source: 119 files, SHA256
`2b85768ef6acf9d2427e03a240d062c114d935bc837cbd16729a4d3a758050f7`.
Build directory names still include r1; the source manifest and executable hashes
identify the actual revision.

Runtime R4 returned snapshot, same-frame detection, model choice, same-frame VQA
and detector restoration, then exited with code 0 and physical cleanup acknowledgement.
Its overall Agent check **failed**: the model unnecessarily chose start_capture
while already running and the common state machine returned STATE_CONFLICT.
The earlier repeated get_status and OOM failures are retained. See the single
[engineering journal](debugging_journal.md#ev-vlm-003--2026-11-16--串行卸载仍需核对实际分配与模型语义).

R4 recorded two process CUDA-context releases and sampled MemAvailable down to
158,412KiB. That is system availability, not allocated GPU bytes, and remains
insufficient evidence for a safe long-running resource budget. RSS/PSS, swap and
sampling times remain separate; these short runs do not establish formal interference.

## Model and tool contracts

Qwen3-VL-2B architecture is served through an owned adapter linked to the audited
existing EdgeQwen SDK/static libraries and device-link object. Actual cached
engine, config, plugin and library hashes are retained. The engine's complete
checkpoint/export lineage is not reconstructed; a matching local base-weight
hash alone does not prove the cached engine was exported from that checkpoint.
Old project performance and stability claims are not inherited.

registered-tool-id-v2 asks the real model for one tool ID. Host code constructs
schema-bound arguments; it does not match natural-language keywords to a canned
execution plan. zh-next-operation-v3 records the current prompt revision. Format
validity, correct tool choice, semantic goal completion and answer grounding must
be evaluated separately. Prompt-only experiments execute no tools and never count
as the 22-task acceptance suite.

Detector boxes/counts/confidences come from the fixed detector. VLM descriptions
are marked local_vlm and do not become detection boxes or distance estimates.
Reports use actual completed receipts and the owned snapshot; private images,
full bags, model weights and engines stay out of Git. Final camera imagery must
be inspected privately before making scene-quality claims.

The development CLI is `python3 -m edgevision_agent` with the project `agent/`
directory on PYTHONPATH and the matching ROS overlay sourced. It requires explicit
snapshot/report roots. Its natural-language workflow remains experimental;
do not treat a successful build or a valid model tool ID as a completed user task.
