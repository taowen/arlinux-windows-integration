# Windows exception observer

The standalone observer and original-handler regression probe were extracted
from the project-owned historical GE investigation recorded in sources.json.
Only these diagnostics are maintained here; no Proton distribution is built,
patched or installed. The observer uses Windows debugging APIs without writing
thread contexts or injecting code. Wine integration builds this source on the
owning repository's runner and distributes it as an optional diagnostic product.

`message-wait-ARCH.exe [audio|noaudio] [keep|drain] [STYLE]` observes a hidden window's bounded
WaitAll startup loop, optionally initializing DirectSound on a worker thread.
It records the actual wait return, queue bits, pending messages and audio result.
The optional `drain` removes creation messages before starting the worker.
Run the identical i686 or x86_64 binary on native Windows and Bionic Wine;
the ARM64 binary isolates translation separately. Each run owns and removes its
window, event and audio objects. No application is injected or modified. A zero
exit means observation completed, not that a game passed or a timeout is a bug.
Styles are `framed` (default), `plain`, `popup`, `popup-caption`, `popup-border`
and `message`. Compare the posted message ID and parameters, not just whether
the wait returned. Retain the negative drain case: consuming an initial message
must not let a subsequent WaitAll complete with only the worker event signaled.
Append `inside-create` after the style to observe a signaled-event WaitAll from
inside the window procedure's `WM_CREATE` callback, before CreateWindow returns.
This distinguishes a missing creation notification from one queued too late.
