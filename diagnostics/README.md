# Windows exception observer

The standalone observer and original-handler regression probe were extracted
from the project-owned historical GE investigation recorded in sources.json.
Only these diagnostics are maintained here; no Proton distribution is built,
patched or installed. The observer uses Windows debugging APIs without writing
thread contexts or injecting code. Wine integration builds this source on the
owning repository's runner and distributes it as an optional diagnostic product.

`message-wait-ARCH.exe [audio] [drain]` observes a hidden window's bounded
WaitAll startup loop, optionally initializing DirectSound on a worker thread.
It records the actual wait return, queue bits, pending messages and audio result.
The optional `drain` removes creation messages before starting the worker.
Run the identical i686 or x86_64 binary on native Windows and Bionic Wine;
the ARM64 binary isolates translation separately. Each run owns and removes its
window, event and audio objects. No application is injected or modified. A zero
exit means observation completed, not that a game passed or a timeout is a bug.
