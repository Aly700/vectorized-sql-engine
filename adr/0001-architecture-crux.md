# ADR 0001: Start from the architectural crux

## Status

Accepted.

## Decision

This project is scaffolded so that the first implementation work happens at the correctness boundary, not in peripheral tooling.

## Consequences

- Tests should focus on invariant preservation.
- Nondeterminism must be isolated behind explicit interfaces.
- Performance decisions must be justified by measurements after the correctness model exists.
