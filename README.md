# SimpleMail

Group Details:

Member 1: Sumedh Jamadagni, 24CSB0A76

Member 2: Tarun Nagarajan 24CSB0A79

SimpleMail implements the networking assignment's three C programs: a concurrent TCP server, a shared-memory spooler, and an interactive client. SMTP2 sends mail without authentication; SMP authenticates users before mailbox access. The implementation uses C11, POSIX sockets/shared memory, process-shared pthread synchronization, and `poll()`.

## Build and run

Use Ubuntu with GCC and Make. All commands below run from the project directory. Start the spooler and server from the same directory so they share the mailbox location.

```sh
make clean
make
```

Run each program in a separate terminal, in this order:

```sh
./smspooler users.txt
./smserver 2525 users.txt
./smclient 127.0.0.1 2525
```

The spooler must be ready before the server starts. Stop the server first, then the spooler. The server's port is configurable; the client takes the server address and port.

The supplied sample accounts are `alice password1`, `bob secret2`, and `carol hunter7`. Usernames are case-insensitive ASCII letters, at most 20 characters. Passwords are case-sensitive ASCII letters/digits, at most 30 characters. Both daemons validate the registry at startup and exit if it is missing or malformed.

## Use the client

Choose `1` to send mail, enter a display name, enter recipients one at a time, and submit an empty recipient line to finish the list. Enter a subject and body; a line containing only `.` finishes body entry. At least one recipient must be accepted. An empty subject becomes `(no subject)`. The client handles SMTP2 chunks and byte credits.

Choose `2` to authenticate and open the mailbox menu: `L` lists, `R` reads, `D` deletes, `C` counts, and `Q` logs out. Logout returns to the main menu; `3` exits. Each action opens a fresh connection because the protocol mode is fixed per connection.

## Required components

| Component | Implementation |
| --- | --- |
| Concurrent frontend | `smserver.c`: nonblocking sockets, `poll()`, per-client input/output buffers and protocol state; one listening port, up to 128 clients. |
| Shared-memory spool | `ipc.[ch]`: eight complete-message slots, ring indexes/count, ticket counter, process-shared mutex and condition variables. |
| Disk spooler | `smspooler.c`: creates user directories, consumes the ring, writes message files and durable ID metadata. The server does not write mail files. |
| Sending protocol | FROM, TO, SUB, BODY_STREAM/CHUNK/END_BODY, legacy BODY/dot-stuffing, repeated transactions and QUIT. |
| Flow control | Initial 2,048-byte credit; chunks consume credit; WINDOW_UPDATE restores staged bytes when spool capacity permits. A full ring can hold the window at zero. |
| Retrieval protocol | Random eight-character nonce, DJB2(password + nonce), three authentication attempts, LIST/READ/DELETE/COUNT/QUIT. |
| Client and helpers | `smclient.c` provides the menu; `common.[ch]` provides registry handling, hashing, nonce generation, logging and byte I/O. |
| Build/documentation | Makefile builds all three executables; `users.txt` supplies sample users; the guide explains synchronization, flow control and concurrency. |

## Storage and limits

Messages are stored as `mailboxes/<user>/<id>.txt`, with From, To, Subject and Date headers, a `---` separator, and the body. `.next-id` reserves IDs before writing mail, so deleting mail or restarting the daemons does not reuse IDs. Missing or invalid metadata in an existing mailbox causes an error instead of guessed history.

SMTP2 command lines are at most 512 bytes including CRLF; bodies are at most 65,536 bytes. This implementation accepts up to 16 recipients, 128 registered users, and 128-byte sender display names. The shared-memory object is `/simplemail_v1_ring` and the queue has eight slots. Queue tickets increase within a spooler run; mailbox IDs persist across runs. `OK Queued` confirms queue acceptance, before the spooler finishes disk delivery.

Daemon events use `[YYYY-MM-DD HH:MM:SS]` prefixes on stdout. The server records connections, modes, authentication, queueing, mailbox operations and disconnections; the spooler records dequeue and delivery results.

## Verification and documentation

The relocated project was built and exercised on Ubuntu 24.04.3 LTS with GCC 13.3.0. Ten local integration tests pass. Additional captured runs force invalid startup, unknown/idle modes, bad command order, failed authentication, body and command limits, zero credits, a full queue, 20 concurrent senders, and deletion followed by restart. The real interactive client and daemon logs are also captured.

The [Markdown documentation](docs/SimpleMail-Technical-Guide.md) covers the required behavior and contains the captured outputs. The [PDF documentation](docs/SimpleMail,%20Documentation.pdf) contains black-and-white terminal renderings. Captured transcripts are in [docs/terminal-output](docs/terminal-output). They reproduce actual Ubuntu output; they are not desktop captures.

Test scripts remain local and are excluded from GitHub. In the development checkout, `make test` runs them. A public clone contains the application, documentation, transcripts and images, but not the private test scripts.

## Common startup problems

If the server cannot open the shared queue, start the spooler first. If binding fails, choose an unused port. For registry errors, check the username/password character and length rules. If ID metadata is reported unsafe, preserve the mailbox and restore its metadata from a valid backup; do not reset the counter. A full queue delays queue confirmation and credit restoration until the consumer resumes.
