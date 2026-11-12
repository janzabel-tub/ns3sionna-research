# Documentation lookup

Use Context7 when available for external library/API documentation, dependency
setup and version-specific code examples. Resolve library IDs before querying;
request the versions pinned in this repository.

For ns3sionna MIMO, use the indexed MIMO fork and `feature/su-mimo-minimal`
branch, rather than assuming upstream `tkn-tub/ns3sionna` includes these changes. Read
`doc/context7.md` for setup, the English API entry point and source registration.
The intended public library is `/janzabel-tub/ns3sionna-research` once indexed;
reject unrelated search matches.
Use the actual library ID returned after indexing; private-source IDs may differ
from public repository IDs.

Check retrieved examples against local headers and examples. Uncommitted or
unpushed changes are not in Context7. If Context7 is unavailable or the fork is
not indexed, use local documentation and source, and state that limitation.
