# File I/O Semantics

VIO file operations use the bounded blocking executor until a platform backend
can provide a better asynchronous file provider.

Cancellation is a request. If a file operation has not started, cancellation can
prevent it from being submitted. Once a blocking system call has started, VIO
reports the actual system result because the side effect may already have
occurred. A completed write, truncate, allocation hint, or sync is never masked
as cancelled.

`sync_data` and `sync_all` flush the current file stream in the portable M5
implementation. Platform-specific durability guarantees are documented by later
backends because operating systems differ in metadata and device-cache behavior.

`sendfile_view` borrows a file without transferring raw native handle ownership.
The source file must outlive the view.
