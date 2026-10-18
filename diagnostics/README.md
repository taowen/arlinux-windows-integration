# Windows exception observer

The standalone observer and original-handler regression probe were extracted
from the project-owned historical GE investigation recorded in sources.json.
Only these diagnostics are maintained here; no Proton distribution is built,
patched or installed. The observer uses Windows debugging APIs without writing
thread contexts or injecting code. Wine integration builds this source on the
owning repository's runner and distributes it as an optional diagnostic product.
