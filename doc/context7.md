# ns3sionna MIMO and Context7

Context7 retrieves library documentation and code examples for AI coding
assistants through MCP (Model Context Protocol). It supports development of
ns3sionna; the simulation itself does not need to call Context7.

There are two independent steps: connect the assistant to Context7, and index
the documentation for the fork containing the MIMO implementation.

## Connect the coding assistant

In ChatGPT Work, install the Context7 plugin and complete its sign-in flow.
In a local Codex client, the manual hosted-MCP alternative is:

```shell
codex mcp add context7 --url https://mcp.context7.com/mcp/oauth
codex mcp login context7
codex mcp list
```

Use either the plugin or manual MCP setup. OAuth requires signing in to your
Context7 account. Start a new Codex thread after installing the plugin so its
tools and skill load. The repository's `AGENTS.md` requests documentation
lookups when Context7 is available. Registration alone does not prove that
documentation queries work; verify with a query after authentication.

See the [Context7 Codex guide](https://context7.com/docs/clients/codex) and
[official OpenAI MCP documentation](https://developers.openai.com/codex/mcp).

## Index this MIMO fork

The Git repository root is `contrib/sionna`, not the enclosing ns-3 tree.
The public MIMO fork is
[`janzabel-tub/ns3sionna-research`](https://github.com/janzabel-tub/ns3sionna-research).
Its default branch is `main`; the maintained MIMO branch is
`feature/su-mimo-minimal`, including the validated Sionna 2.2.0 update from
`Sionna220`.
Do not submit the upstream repository expecting it to contain this fork's
changes.

1. Commit and push the documentation and `context7.json` to the intended
   repository. Context7 cannot read local files or unpushed commits.
2. For a public repository, submit its URL at
   [Add Library](https://context7.com/add-library). Make `context7.json`
   available on the default branch so Context7 can discover the configuration;
   its `branch` field selects `feature/su-mimo-minimal` for parsing.
3. For a private repository, use your Context7 teamspace's **Sources** tab,
   select GitHub, authorize repository access, and submit the source. Context7
   currently requires a Pro or Enterprise plan for
   [private sources](https://context7.com/docs/howto/private-sources).
4. Wait for indexing, copy the actual library ID, and test a MIMO query. For
   the public fork above, the expected ID is
   `/janzabel-tub/ns3sionna-research`; private-source IDs may differ.
5. Refresh the source after publishing significant documentation changes.

The root `context7.json` selects the MIMO branch and excludes measurement
artifacts and implementation directories. Context7 indexes documentation and
the code examples embedded in it; merely including a C++ header directory does
not provide complete API documentation. Context7 currently skips pages whose
prose is mainly outside English, so this page is an English entry point for
the German [full MIMO model](../ns3sionna_mimo_model.md). It is not a complete
translation of that model.

Configuration and parsing behavior are described in
[Library Owners](https://context7.com/docs/library-owners) and
[Adding Libraries](https://context7.com/docs/adding-libraries).

## MIMO architecture and versions

This fork targets the project's modified ns-3.40 tree. Installing the contrib
module into an otherwise stock ns-3 tree does not supply its Wi-Fi changes.
The Python server pins Sionna 2.2.0, Sionna RT 2.2.0, PyTorch 2.9.1,
Mitsuba 3.9.1 and Dr.Jit 1.5.0. See the root [installation guide](../README.md)
and `model/ns3sionna/requirements.txt` for environment setup.

Sionna RT computes propagation loss, delay and complex channel frequency
responses. Protobuf/ZMQ transfers these to ns-3. ns-3 handles precoding,
detection, stream SINR, CSI feedback/aging and packet reception. For subcarrier
`k`, `H[k]` has shape `[numRxAntennas, numTxAntennas]`. The serialized tensor
uses `[subcarrier, rx, tx]` order and index
`(k * numRxAntennas + r) * numTxAntennas + t`.

Supported helper modes are `SISO`, `SU_MIMO_SPATIAL_MULTIPLEXING`,
`SU_BEAMFORMING` and `MU_MIMO`. Open-loop SU reception uses linear MMSE.
SU beamforming supports explicitly selected MRT or SVD under the helper's
validation constraints. HE downlink MU needs explicit runtime configuration
and `InstallDownlink`; selecting `MU_MIMO` alone is insufficient. See the
[channel abstraction](sionna-ns3-abstraction.md) and the full model for limits.

## Minimal 2-by-2 SU configuration

This fragment assumes existing `SionnaHelper`, TX/RX
`SpectrumWifiPhyHelper` objects and a
`Ptr<SionnaSpectrumPropagationLossModel>`, named as shown below. Apply the
configuration before installing devices, following the complete examples.

```cpp
#include "ns3/sionna-mimo-helper.h"

ns3::SionnaMimoConfiguration configuration;
configuration.mode = ns3::SionnaMimoMode::SU_MIMO_SPATIAL_MULTIPLEXING;
configuration.txArray.numColumns = 2;
configuration.rxArray.numColumns = 2;
configuration.numTxAntennas = 2;
configuration.numRxAntennas = 2;
configuration.maxNss = 2;

ns3::SionnaMimoHelper mimo(configuration);
mimo.ApplyForHeMcs(0, sionna, txPhy, rxPhy, spectrumModel);
```

The current helper defaults to `MEAN_UNCODED_BER`. If selecting `EESM`
explicitly, supply a positive, calibrated `eesmBetaLinear` entry for every
used HE MCS. The historical minimal example explicitly selects EESM.
Implemented modes and general BER mapping do not imply measured PER
calibration for every antenna/MCS combination. The documented pilot
calibration covers HE MCS 0, BCC, 32-byte packets and a 1-by-1 channel.

Complete examples are
[`ns3_wifi_sumimo_minimal.cc`](../examples/ns3_wifi_sumimo_minimal.cc),
[`ns3_wifi_sumimo_precoding.cc`](../examples/ns3_wifi_sumimo_precoding.cc), and
[`ns3_wifi_mumimo.cc`](../examples/ns3_wifi_mumimo.cc). From the ns-3 directory,
with the project's dependencies installed:

```shell
./ns3 configure --enable-examples --enable-tests
./ns3 build ns3_wifi_sumimo_minimal ns3_wifi_sumimo_precoding ns3_wifi_mumimo
```

## Example assistant prompts

After indexing, replace `LIBRARY_ID` with the returned ID:

```text
Use Context7 with LIBRARY_ID to configure a 2x2 SU-MIMO link on the
feature/su-mimo-minimal branch, using ns-3.40 and Sionna 2.2.0. Check the example against
the local SionnaMimoHelper header.

Use Context7 with LIBRARY_ID to explain CFR serialization order and the
requirements for HE downlink MU-MIMO.
```

Resolve IDs for external dependencies such as Sionna separately, specifying
the pinned versions. Context7 can only return versions that are indexed;
use the installed source when the required version is unavailable.
