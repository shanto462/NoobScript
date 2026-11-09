# Security Policy

## Supported versions

Security fixes go to the latest release on `main`.

## Reporting a vulnerability

Please don't open a public issue. Use GitHub's [private vulnerability reporting](https://github.com/shanto462/NoobScript/security/advisories/new) instead, with the smallest script that reproduces the problem.

## What counts

NoobScript is not a sandbox. A script runs with your user's permissions and can read and write files, so running an untrusted script is as risky as running any other untrusted program.

Bugs worth reporting include memory corruption or a crash in the interpreter caused by a script. Those should always be a NoobScript error instead.
