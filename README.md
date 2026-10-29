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

| Component | Files | Implementation and behavior |
| --- | --- | --- |
| Concurrent frontend | `smserver.c` | Opens one configurable TCP listening port and handles up to 128 clients using nonblocking sockets and `poll()`. Each client has its own input/output buffers, message record, protocol state and authentication state. A slow sender or a full spool queue does not block unrelated sessions. |
| Connection modes and framing | `smserver.c` | Sends the welcome greeting, accepts `MODE SEND` or `MODE RECV`, rejects unknown modes, and disconnects clients that do not select a mode within 30 seconds. Commands are assembled across partial TCP reads; chunk payloads are read using their announced byte counts. |
| Shared-memory spool | `ipc.c`, `ipc.h` | Provides the POSIX shared-memory object `/simplemail_v1_ring`, containing an eight-slot circular queue. Each slot stores a complete message: sender display name, accepted recipients, subject, body bytes and length, and a queue ticket. Head, tail and count track occupied slots. |
| Process synchronization | `ipc.c`, `ipc.h` | Uses a process-shared pthread mutex and condition variables to protect queue records, indexes and counters. The spooler waits for messages; the frontend attempts insertion without blocking its event loop. When the ring is full, a completed message stays in that client's staging record until space is available. |
| Disk spooler | `smspooler.c` | Loads the registry, creates a mailbox directory for every registered user, and consumes messages from the ring. It writes one `<id>.txt` copy to each accepted recipient, including From, To, Subject and Date headers, the separator and body. The frontend does not create or write mail files. |
| Durable mailbox IDs | `smspooler.c` | Maintains a separate `.next-id` counter for each mailbox. IDs are reserved before mail files are written, and metadata is synchronized to disk. Deleted IDs are not reused after restart. Missing or malformed metadata in an existing mailbox causes an error rather than a guessed counter. |
| SMTP2 sending protocol | `smserver.c` | Enforces the order FROM, one or more accepted TO commands, SUB, and a body mode. Sending requires no login. Unknown recipients are rejected, empty subjects become `(no subject)`, and bad command order returns `ERR Bad sequence`. A queued transaction returns its ticket and recipient count; another FROM can start the next message. |
| Body transfer and flow control | `smserver.c`, `smclient.c` | Supports `BODY_STREAM`, `CHUNK <byte_count>` and `END_BODY`, plus legacy dot-terminated BODY with dot-stuffing. Streaming starts with 2,048 byte credits. Chunks consume credit, and `WINDOW_UPDATE` restores staged bytes when queue capacity permits. Zero credits pause sending; an overrun closes the connection. Both modes enforce the 65,536-byte body limit. |
| SMP authentication | `smserver.c`, `common.c` | Generates a random eight-character alphanumeric nonce after RECV selection. The client sends the unsigned-decimal DJB2 hash of password followed by nonce, without a separator. Username matching ignores case; passwords preserve case. A successful login fixes the mailbox identity, and the third failed attempt closes the connection. |
| Mailbox operations | `smserver.c` | Implements LIST with ID/sender/subject/date rows, READ with message contents and transport dot-stuffing, DELETE, COUNT and QUIT. LIST and READ use a single-dot terminator. Missing message IDs and unknown commands return errors; deletion preserves the mailbox's ID history. |
| Interactive client | `smclient.c` | Provides the numbered send/check-mailbox/quit menu. Sending prompts for display name, recipients, subject and body while handling chunks and credit updates. The mailbox menu supports list, read, delete, count and logout, and removes transport dot-stuffing from displayed mail. A fresh connection is used for the next mode after returning to the main menu. |
| Registry and common helpers | `common.c`, `common.h` | Shares constants and routines for registry validation, canonical usernames, DJB2 hashing, random nonces, timestamped logs and byte I/O. Both daemons reject missing or malformed registries at startup. The same username and password rules are applied consistently across the programs. |
| Event logging | `smserver.c`, `smspooler.c`, `common.c` | Writes stdout events with `[YYYY-MM-DD HH:MM:SS]` prefixes. The server records connections, mode selection, authentication, queue acceptance, mailbox operations and disconnection. The spooler records dequeue and delivery results, so queue acceptance and later disk delivery can be distinguished. |
| Build, sample data and documentation | `Makefile`, `users.txt`, `docs/` | Builds `smserver`, `smspooler` and `smclient` with the configured C compiler and warning flags. The sample registry supplies the three demonstration accounts. The [Markdown guide](docs/SimpleMail-Technical-Guide.md) and [PDF documentation](docs/SimpleMail,%20Documentation.pdf) explain the architecture, synchronization, protocols, storage and client operation. |

## Storage and limits

Messages are stored as `mailboxes/<user>/<id>.txt`, with From, To, Subject and Date headers, a `---` separator, and the body. `.next-id` reserves IDs before writing mail, so deleting mail or restarting the daemons does not reuse IDs. Missing or invalid metadata in an existing mailbox causes an error instead of guessed history.

SMTP2 command lines are at most 512 bytes including CRLF; bodies are at most 65,536 bytes. This implementation accepts up to 16 recipients, 128 registered users, and 128-byte sender display names. The shared-memory object is `/simplemail_v1_ring` and the queue has eight slots. Queue tickets increase within a spooler run; mailbox IDs persist across runs. `OK Queued` confirms queue acceptance, before the spooler finishes disk delivery.

Daemon events use `[YYYY-MM-DD HH:MM:SS]` prefixes on stdout. The server records connections, modes, authentication, queueing, mailbox operations and disconnections; the spooler records dequeue and delivery results.

## Output

### Build and regression results (1/2)

![Build and regression results (1/2)](docs/terminal-output/verification-01.png)

### Build and regression results (2/2)

![Build and regression results (2/2)](docs/terminal-output/verification-02.png)

### Registry errors and startup order

![Registry errors and startup order](docs/terminal-output/startup.png)

### Unknown mode and idle timeout

![Unknown mode and idle timeout](docs/terminal-output/handshake.png)

### Sequence and recipient validation

![Sequence and recipient validation](docs/terminal-output/recipients.png)

### Default subject and dot stuffing (1/2)

![Default subject and dot stuffing (1/2)](docs/terminal-output/legacy-01.png)

### Default subject and dot stuffing (2/2)

![Default subject and dot stuffing (2/2)](docs/terminal-output/legacy-02.png)

### Authentication and mailbox errors

![Authentication and mailbox errors](docs/terminal-output/authentication.png)

### Body limit and window overrun (1/7)

![Body limit and window overrun (1/7)](docs/terminal-output/limits-01.png)

### Body limit and window overrun (2/7)

![Body limit and window overrun (2/7)](docs/terminal-output/limits-02.png)

### Body limit and window overrun (3/7)

![Body limit and window overrun (3/7)](docs/terminal-output/limits-03.png)

### Body limit and window overrun (4/7)

![Body limit and window overrun (4/7)](docs/terminal-output/limits-04.png)

### Body limit and window overrun (5/7)

![Body limit and window overrun (5/7)](docs/terminal-output/limits-05.png)

### Body limit and window overrun (6/7)

![Body limit and window overrun (6/7)](docs/terminal-output/limits-06.png)

### Body limit and window overrun (7/7)

![Body limit and window overrun (7/7)](docs/terminal-output/limits-07.png)

### TCP fragments and line length (1/2)

![TCP fragments and line length (1/2)](docs/terminal-output/framing-01.png)

### TCP fragments and line length (2/2)

![TCP fragments and line length (2/2)](docs/terminal-output/framing-02.png)

### Full queue, zero window and recovery (1/5)

![Full queue, zero window and recovery (1/5)](docs/terminal-output/backpressure-01.png)

### Full queue, zero window and recovery (2/5)

![Full queue, zero window and recovery (2/5)](docs/terminal-output/backpressure-02.png)

### Full queue, zero window and recovery (3/5)

![Full queue, zero window and recovery (3/5)](docs/terminal-output/backpressure-03.png)

### Full queue, zero window and recovery (4/5)

![Full queue, zero window and recovery (4/5)](docs/terminal-output/backpressure-04.png)

### Full queue, zero window and recovery (5/5)

![Full queue, zero window and recovery (5/5)](docs/terminal-output/backpressure-05.png)

### Twenty concurrent senders (1/10)

![Twenty concurrent senders (1/10)](docs/terminal-output/concurrency-01.png)

### Twenty concurrent senders (2/10)

![Twenty concurrent senders (2/10)](docs/terminal-output/concurrency-02.png)

### Twenty concurrent senders (3/10)

![Twenty concurrent senders (3/10)](docs/terminal-output/concurrency-03.png)

### Twenty concurrent senders (4/10)

![Twenty concurrent senders (4/10)](docs/terminal-output/concurrency-04.png)

### Twenty concurrent senders (5/10)

![Twenty concurrent senders (5/10)](docs/terminal-output/concurrency-05.png)

### Twenty concurrent senders (6/10)

![Twenty concurrent senders (6/10)](docs/terminal-output/concurrency-06.png)

### Twenty concurrent senders (7/10)

![Twenty concurrent senders (7/10)](docs/terminal-output/concurrency-07.png)

### Twenty concurrent senders (8/10)

![Twenty concurrent senders (8/10)](docs/terminal-output/concurrency-08.png)

### Twenty concurrent senders (9/10)

![Twenty concurrent senders (9/10)](docs/terminal-output/concurrency-09.png)

### Twenty concurrent senders (10/10)

![Twenty concurrent senders (10/10)](docs/terminal-output/concurrency-10.png)

### IDs after deletion and restart (1/2)

![IDs after deletion and restart (1/2)](docs/terminal-output/restart-01.png)

### IDs after deletion and restart (2/2)

![IDs after deletion and restart (2/2)](docs/terminal-output/restart-02.png)

### Interactive send menu

![Interactive send menu](docs/terminal-output/client-send.png)

### Interactive mailbox menu

![Interactive mailbox menu](docs/terminal-output/client-mailbox.png)

### Timestamped server and spooler events

![Timestamped server and spooler events](docs/terminal-output/daemons.png)
