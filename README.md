# SimpleMail

Group members: Sumedh Jamadagni (24CSB0A76) and Tarun Nagarajan (24CSB0A79).

SimpleMail is a small POSIX mail system with a polling TCP server, a separate mailbox spooler, and an interactive terminal client. It is intended for Linux and other POSIX systems with POSIX shared memory and process-shared pthread synchronization.

Build and start the spooler from the project directory:

```sh
make
./smspooler users.txt
```

In a second terminal, start the server. In a third terminal, open the client:

```sh
./smserver 2525 users.txt
./smclient 127.0.0.1 2525
```

The spooler must start before the server. The sample registry contains `alice`, `bob`, and `carol`; each line holds a username and password separated by whitespace. Usernames are ASCII letters, up to 20 characters, and case-insensitive. Passwords are ASCII letters or digits, up to 30 characters, and case-sensitive.

Mailboxes are created under `mailboxes/<username>/`. Each delivered message is a numbered `.txt` file. A hidden `.next-id` file preserves the next mailbox ID across deletion and restart. Daemon events go to stdout with `[YYYY-MM-DD HH:MM:SS]` timestamps. Stop the server first, then the spooler.

The spooler only initializes ID metadata for a newly created, empty mailbox. If an existing mailbox has missing or malformed metadata, it stops instead of guessing and risking ID reuse.

Run the local end-to-end suite with `make test`. See [docs/SimpleMail-Technical-Guide.md](docs/SimpleMail-Technical-Guide.md) for architecture, protocol formats, flow control, assumptions, and test coverage.

The [PDF technical guide](docs/SimpleMail,%20Documentation.pdf) includes monochrome terminal renderings of captured Ubuntu build, test, and protocol output. Test scripts are retained only in the local development checkout and are excluded from the public repository.
