# Execution cadence

## Contents

1. Purpose
2. Inner loop
3. Readiness review
4. Work-package checkpoint
5. Phase certification
6. Risk escalation

## 1. Purpose

Preserve the final proof standard while keeping ordinary implementation feedback fast. A cadence controls when a command is appropriate; a risk class controls which narrow proof must run.

## 2. Inner loop

Use for one dependency-coherent implementation slice.

Required:

- failing or existing narrow oracle;
- dedicated Debug build target;
- filtered unit/contract tests;
- changed-file formatter/static checks;
- `git diff --check`.

Forbidden unless the user explicitly requests it:

- full Release/LTO engine or monolithic unit-test link;
- whole local suite or broad CI;
- artifact manifests, per-file hashes, sidecars, packaging;
- fuzz, loss, performance, or soak campaigns;
- gate closure or checklist mutation;
- reviewer ceremony for a helper-sized change.

Target a feedback cycle below two minutes when the repository permits it. A slower target is a build-topology problem to record and narrow, not a reason to repeat it after every edit.

## 3. Readiness review

Run once when the parent work package is cohesive and its targeted tests are green.

The independent reviewer inspects:

- owning contract and inherited invariants;
- complete focused production and test diff;
- negative, disabled, cleanup, overload, visibility, and boundary behavior;
- narrow raw test results;
- proposed checkpoint command set.

Return all blocking findings in one batch. Do not build Release evidence or rewrite a gate report before readiness returns no blocking finding.

## 4. Work-package checkpoint

After readiness approval:

1. build the changed production/test targets in Release;
2. run unit/contract and directly affected integration/lifecycle tests;
3. run one broad suite when shared runtime, protocol, build integration, or common harness changed;
4. rerun only proofs invalidated by a fix;
5. bind conclusive evidence once;
6. reconcile the compliance matrix and progress tracker;
7. perform a focused disposition/evidence re-review.

A checkpoint may establish `implemented` or `verified` work-package status. It does not imply phase certification.

## 5. Phase certification

Use only for a final or certification-only gate, a user-requested release claim, or a campaign assigned by the contract. An ordinary intermediate WP gate uses the checkpoint cadence unless its contract explicitly assigns certification evidence. Run the exact required variants, independent decoder/goldens, hostile corpus, fuzz, loss, performance, soak, packaging, licensing, platform, and review evidence. Do not substitute abbreviated inner-loop results.

## 6. Risk escalation

| Risk | Immediate narrow proof | Full certification trigger |
|---|---|---|
| wire/schema/vector | byte/golden/decoder test | any public byte or registry change |
| hostile input/security | negative parser/quota test | gate assigns fuzz/corpus or boundary changes |
| visibility/identity | leak oracle and forbidden-field scan | trust-boundary or public identity change |
| thread/ownership | deterministic thread/lifetime test | cross-thread ownership change |
| lifecycle/data loss | restart/cleanup/reorder reproducer | gate assigns loss/soak campaign |
| memory/allocation | exact/+1 and allocation oracle | quota change or performance gate |
| build/platform | changed dedicated target | packaging/platform gate |

A finding expands the immediate proof in its row. It does not pull unrelated certification campaigns into the inner loop.
