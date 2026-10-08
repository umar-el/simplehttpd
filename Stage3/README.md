# Stage 3: tests and demo

Tests and a demo for the Stage 2 server (and a short check of Stage 1).

```bash
make test        # unit tests, then integration tests
make unit        # unit tests only
make integration # integration tests only
./demo.sh        # step-by-step walkthrough (DEMO_NOPAUSE=1 to run straight through)
```

- `tests/test_units.c` links the Stage 2 sources directly. It tests the parser, `read_request` over a
  `socketpair`, path resolution against a temporary folder, escaping and `serve_path`.
- `tests/integration.sh` builds both stages (Stage 2 with `-fsanitize=address,undefined`), starts each server on
  port 18080 against a temporary site, and checks it with `curl` and raw `/dev/tcp` connections.

Everything is built with `-fno-sanitize-recover=all`, and the integration test fails if the server writes any
sanitizer report, so memory errors and undefined behaviour fail the run.
