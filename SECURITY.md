# Security

The inspection commands (`inspect`, `assess`, `map`, `bind`, `inspect-package`,
`inspect-config`) treat their input as untrusted and never execute it. Crashes
or memory errors in those are bugs, please report them.

`xvalkyrie exec`, and `xvalkyrie <file>` once a title is runnable, execute the
file's code inside the xvalkyrie process with your user's privileges. There is
no sandbox. Only run files you trust.

To report a vulnerability use GitHub's private vulnerability reporting on this
repository. Please do not attach proprietary binaries.
