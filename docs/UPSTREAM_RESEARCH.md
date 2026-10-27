# Upstream compatibility research

Checked 2026-10-27. Primary sources only: repository source/history, first-person
issue reports, the author's build guide, and Apple documentation. Reports below
are evidence of what their authors observed, not independent verification on our
Quest or proof that superficially similar failures share a cause.

## Findings that change the next steps

1. **A newer transport patch exists, but its useful PID changes are already in
   this port.** Commit `b743dc2` from 2025-11-02 adds `0x5012` and `0x5013` in the
   interface constants, driver switch and prober table. It is titled as a fix
   for Quest 2 v81 PTC, so these IDs should not be described as exclusively Quest
   3 identifiers. Its other changes concern libuvc discovery and an H.264
   submodule; it contains no simulator capture or shared-event fix.
   [Exact commit](https://github.com/shinyquagsire23/monado/commit/b743dc2f6993aad2b585f15733d37511954b5815).

2. **One-view simulator rendering is documented behavior.** Apple says the view
   count depends on the environment and Simulator ordinarily renders one view.
   Respect the returned count and texture topology; forcing the getter to two
   does not establish a second allocated view. This supports preserving the
   native frame and initially duplicating its captured image for OpenXR eyes.
   This is an API-contract correction, not proof that it alone cures our stall.
   [Apple view-count documentation](https://developer.apple.com/documentation/compositorservices/cp_drawable_get_view_count).

3. **No verified contemporary XRGyroControls capture fix was found.** The public
   issue list has an unresolved fully immersive Metal black-screen report and a
   later Xcode 16.2/Quest Link failure. Neither supplies a fix for the current
   pre-OpenXR `SharedEventSignalBuffer` GPU timeout. Treat texture ownership,
   command-buffer ordering and pool starvation as local hypotheses to test,
   rather than citing them as known upstream bugs.
   [Metal issue #5](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/issues/5),
   [Xcode 16.2 issue #7](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/issues/7).

## Upstream baseline and newer forks

The upstream README's tested combination is macOS Sonoma 14, Xcode 15.1 beta 2
and M1 machines. Its latest main commit is dated 2023-11-14; the last code fix
that day is specifically for beta 5. A successful 2023 release is therefore not
evidence of compatibility with visionOS/Xcode 26 or 27.
[README](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/blob/2aeee660e045e6d679e8b3ebb1ee346e57405132/README.md),
[beta-5 fix](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/commit/a2a1def678297727acd861ea5ab7add3d204b28f).

The complete public fork list returned five forks. Excluding this task's own
`mkellerman` fork, the four other forks and all their advertised branches were
checked. None contained a 2024–2026 compatibility update in those branch heads:

| Fork | Checked branch heads | Latest relevant head date |
| --- | --- | --- |
| [johnpena](https://github.com/johnpena/XRGyroControls_OpenXR/commits/main/) | Same main SHA as upstream | 2023-11-14 |
| [mrhegemon](https://github.com/mrhegemon/XRGyroControls_OpenXR/commits/main/) | Same main SHA; `hax` ends with an older move-to-main notice | 2023-11-14; `hax` 2023-07-07 |
| [KCBF](https://github.com/KCBF/XRGyroControls_OpenXR/commit/b4f5d3d7091ac802c894fcc420fb0d4007b25d90) | Older right-eye/floor and input changes | 2023-07-16 |
| [cualquiercosa327](https://github.com/cualquiercosa327/XRGyroControls_OpenXR/commit/e40bc7dfa9e7e7c2196200b3f03592c885291c50) | Older experimental snapshot | 2023-06-28 |

Evidence scope: [public forks API](https://api.github.com/repos/shinyquagsire23/XRGyroControls_OpenXR/forks?per_page=100&sort=newest)
and the branches of those forks. This does not rule out unadvertised branches,
private forks or unrelated projects. Upstream's all-state pull-request collection
was empty at the check time.
[Pull-request collection](https://api.github.com/repos/shinyquagsire23/XRGyroControls_OpenXR/pulls?state=all&per_page=100).

The author's `macos_ql_merge` Monado branch still ends at `41abf1e` on
2023-10-17. The 2025 PID patch is a separately reachable commit whose parent is
that exact revision, authored by `crystall1nedev`; it is not the current branch
head. No associated upstream PR was returned, and the presumed author's public
`monado` fork URL returned 404. Use the exact accessible commit as the reference,
not a claim of a maintained newer branch.
[Branch history](https://github.com/shinyquagsire23/monado/commits/macos_ql_merge/),
[PID patch](https://github.com/shinyquagsire23/monado/commit/b743dc2f6993aad2b585f15733d37511954b5815).

## Confirmed reports versus available fixes

| Date and source | What is actually reported | Consequence for this port |
| --- | --- | --- |
| 2023-08-12 through 2023-12-04, [issue #3](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/issues/3) | Quest Link starts but stays at three dots; follow-up still fails with the latest release. Closed later as not planned, not with a demonstrated fix. | Similar loading screens do not identify one transport/capture cause. |
| 2023-12-24, [issue #5](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/issues/5) | Fully immersive CompositorServices Metal apps produce black output. No replies/fix in the fetched issue. | Do not promise immersive-app coverage from home-screen capture alone. |
| 2025-03-17, [issue #7](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/issues/7) | Quest Link fails on a modified Xcode 16.2 setup; reporter suspects firmware or launch changes. No maintainer diagnosis or fix. | Confirms a compatibility report, not the reporter's suspected root cause. |
| 2026-01-19, [Monado issue #3 comment](https://github.com/shinyquagsire23/monado/issues/3#issuecomment-3770322068) | Tester applies the 2025 PID patch on M1 Ultra with Vulkan SDK 1.4.313.1 and reports an image, hand/controller movement and vibration. Also reports distortion, poor smoothness and 400/800-byte pipe errors. | Useful current transport evidence; no proof of visionOS Simulator capture compatibility or error-free streaming. |
| 2026-02-08, [build-guide comment](https://gist.github.com/shinyquagsire23/3c68aecd872cc7ac21c28e950245dbd2) | Build fails on the two optional eye-orientation side-channel symbols. No demonstrated resolution in the visible comment. | Keep optional side-channel compatibility distinct from USB enumeration and GPU capture. |

## The 400-byte error is not a new unique diagnosis

A first-person log posted on 2023-07-21 already contains repeated
`Failed to send 400 bytes (sent 0)` with USB timeouts while reporting a 5 Gbit/s
link. The 2026 tester above reports the same packet sizes with **pipe errors**
while also seeing video and input. Packet length alone therefore does not prove
a new Quest firmware regression or identify a single remedy. Preserve the actual
libusb error and connection/handshake stage.
[2023 original log](https://gist.github.com/gtk2k/f41c2f88eae8f72f1381bb1d67ee4fc7),
[2026 first-person report](https://github.com/shinyquagsire23/monado/issues/3#issuecomment-3770322068).

The `xrsp_tests` main branch's latest commit is from 2022-11-20. Its only fetched
issue is a 2025 suggestion about additional research opportunities, not a newer
USB timeout or simulator rendering fix. It remains protocol reference material,
not a validated contemporary replacement runtime.
[Commit history](https://github.com/shinyquagsire23/xrsp_tests/commits/main/),
[issue #1](https://github.com/shinyquagsire23/xrsp_tests/issues/1).

## Workarounds worth distinguishing

The upstream README documents judder after headset sleep, controller Home-button
dependence on simulator-window focus, and Menu resetting a stuck controller gaze
ray. It also speculates about MetalSim degradation after repeated launches and
suggests a host restart. These are historical first-party troubleshooting notes;
the speculative MetalSim explanation is not confirmation of our specific GPU
fault. Its old security-setting instructions are not newly required evidence for
this port, which has loaded with SIP enabled.
[Upstream known issues](https://github.com/shinyquagsire23/XRGyroControls_OpenXR/blob/2aeee660e045e6d679e8b3ebb1ee346e57405132/README.md#known-issues).

Monado has real source-supported throughput controls: `QL_OVERRIDE_FPS`,
`QL_OVERRIDE_SCALE`, `QL_OVERRIDE_FB_W` and `QL_OVERRIDE_FB_H`, followed by bitrate
and threads-per-slice additions. These can reduce transport/encoding load once
frames work; they cannot explain or cure a simulator GPU timeout that happens
before OpenXR initialization.
[Resolution/FPS commit](https://github.com/shinyquagsire23/monado/commit/14e40709936ebac3ad2d3e9b74c8f2c5d94509ec),
[bitrate commit](https://github.com/shinyquagsire23/monado/commit/4badd227c845645258eacdbb3f645e374ffe39ab).

Apple's current rendering guide directs render work to a dedicated thread and
recommends preparing expensive pipeline/resources before frame encoding. The
drawable documentation explains that frames/drawables are recycled and should
be acquired only when encoding is ready. Together these support a bounded,
nonblocking capture path with native frame/presentation behavior preserved.
They do not establish an automatic remedy for injected system-compositor code.
[Rendering guide](https://developer.apple.com/documentation/compositorservices/drawing-fully-immersive-content-using-metal),
[drawable lifecycle](https://developer.apple.com/documentation/compositorservices/drawable).

Apple also documents that Simulator translates Metal to the host GPU and has
different feature limits from hardware. Query actual capabilities instead of
assuming a device-style stereo or synchronization implementation exists.
[Metal in Simulator](https://developer.apple.com/documentation/metal/developing-metal-apps-that-run-in-simulator).

## Remaining uncertainty and next experiment

No searched primary source supplies a verified Xcode/visionOS 26–27 fix for the
specific `SharedEventSignalBuffer` capture stall. Exact-string web/GitHub searches
and all upstream issues were checked; this is a bounded negative result, not a
claim that no such fix exists anywhere.

Prioritize the local control experiment: unchanged native rendering, then an
observation-only capture hook, then a bounded bridge-owned copy, then OpenXR
consumption, followed separately by head tracking and controller input. Capture
GPU completion/error and timing at each boundary. Retention, synchronization,
allocation and callback timing remain hypotheses until that comparison isolates
one. Avoid presenting historical loader hacks, a PID patch, or a headset restart
as a verified cure for a pre-OpenXR GPU failure.

## Additional local failures and related platform evidence

These are separately classified observations supplied by the active debugging
session, not newly discovered upstream issue reports:

- **Shared-memory `EINVAL`: concrete platform contract.** Apple's XNU source
  rejects another `ftruncate` after a POSIX shared-memory region is allocated.
  Both local participants previously truncated the same region unconditionally;
  the parent fixed reuse by checking its existing size and verified reopening on
  this Mac. That is a source-supported fix, distinct from USB or rendering.
  [Apple XNU implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/posix_shm.c).
- **Process-launch watchdog: a termination class, not automatically a GPU
  crash.** The parent identified a local `FRONTBOARD` / `0x8BADF00D` launch
  watchdog with a 30-second limit and a blocked main-thread initialization path.
  Apple documents watchdog terminations and investigating thread work that
  prevents lifecycle progress. Moving potentially blocking headset/OpenXR
  initialization outside the compositor critical path is a targeted local
  experiment; the blocked initialization's causal connection still needs the
  runtime comparison.
  [Apple watchdog guidance](https://developer.apple.com/documentation/xcode/addressing-watchdog-terminations).
- **CoreRE shader error about 31 versus 32 buffers: related documented limit,
  no exact matched fix.** Apple's Metal tables specify 31 buffer-argument-table
  entries per function and, for Apple2, at most 31 buffers accessible per stage
  through argument buffers. The simulator's documented Apple2-like capabilities
  make the logged limit plausible. This does not identify which generated
  pipeline requested 32 or prove that bridge code caused it.
  [Metal limits](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf),
  [Simulator translation](https://developer.apple.com/documentation/metal/developing-metal-apps-that-run-in-simulator).

Related RealityKit issue **169054912** concerns soft shadows and spotlight
quality being unavailable while clustering is disabled because of restricted
tier-2 argument-buffer issues. Crucially, the visionOS 27 beta 2/3 release-note
views list it as a known issue, while beta 4 and beta 8 views mark it **fixed**.
It is therefore not evidence that the installed later 27.0 runtime still has
that exact issue, and it does not mention this CoreRE 32-buffer diagnostic or
an injected compositor bridge.
[Earlier known issue](https://developer.apple.com/documentation/visionos-release-notes/visionos-27-release-notes?changes=l_2),
[later fixed issue](https://developer.apple.com/documentation/visionos-release-notes/visionos-27-release-notes?changes=_1_3).

Exact-string primary searches for the 31/32-buffer diagnostic did not produce a
matching published bug/fix in this pass. Do not raise reported GPU capabilities
or argument-buffer limits to suppress it. A useful next comparison is whether
the same pipeline error occurs with capture paused or in a clean native run,
while preserving exact runtime/build and pipeline names.
