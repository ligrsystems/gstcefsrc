# Experimental Linux CUDA output

Build with GStreamer 1.26 CUDA development headers, EGL, and OpenGL development packages.
Use a CEF binary distribution that exports native DMA-BUFs and completes GPU writes before accelerated callbacks.
The tested CEF 152 build includes native allocation and producer-completion changes. Stock CEF 152 did not satisfy that contract on the tested T4.
If CUDA headers are outside standard paths, set `GST_CEF_CUDA_INCLUDE_DIR`.
Official GStreamer CUDA stub headers suffice; no CUDA compiler or direct driver link is required.

```sh
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release \
  -DGST_CEF_ENABLE_CUDA=ON -DGST_CEF_EXTERNAL_ROOT=/absolute/cef_binary
cmake --build build-cuda --parallel 8
./tests/run-portable-tests.sh
```

`GST_CEF_ENABLE_CUDA` defaults to `OFF`.
The external CEF root is independent of CUDA support and also builds the CEF152 CPU comparison arm.

Set `cuda-memory=true` before the element enters READY.
The source then advertises only `video/x-raw(memory:CUDAMemory),format=BGRA`.
A CUDA-enabled build still defaults to ordinary CPU output.
The `gpu` property controls browser rendering and does not select CUDA output.
CEF initializes GPU settings once per process. Set `GST_CEF_GPU_ENABLED=1` before creating mixed CPU/CUDA comparison sources.

Linux CUDA mode defaults to `use-angle=gl-egl` and `ozone-platform=x11` during CEF startup.
The plugin applies these defaults after parsing `chrome-extra-flags` or `GST_CEF_CHROME_EXTRA_FLAGS`.
Explicit switches retain their values. CPU mode does not add these defaults.
The first source initializes CEF for the whole process.
If a CPU source initializes CEF first, also set `GST_CEF_CHROME_EXTRA_FLAGS=use-angle=gl-egl,ozone-platform=x11` before startup.

These defaults require an X11 display and native EGL DMA-BUF import for the allocated buffer's format and modifier.
ANGLE's GLX backend does not expose the required DMA-BUF import extension.
Ozone headless has no native pixmap GL importer in this CEF version.
An unsupported explicit backend can therefore fail before the accelerated paint callback.
Run separate processes for matched performance comparisons.

The hardware examples require external HTML fixtures and full-frame checkers from the integrating application.
Eight portable C++ tests cover layout, buffer ownership, demux ownership, diagnostic pairs, popup layout, startup defaults, normal delivery, and scheduling.
They require a C++14 compiler, pthreads, pkg-config, and GStreamer core, video, audio, and Check development files.
The clock test requires GStreamer Check 1.18 or later for `gst_test_clock_process_id`.
It uses `GstTestClock` without CEF, CUDA, a display, or a GPU.
On Ubuntu, `libgstreamer1.0-dev` supplies `gstreamer-check-1.0.pc`, the clock-test headers, and the Check library.
The existing `libgstreamer-plugins-base1.0-dev` dependency installs that core development package.
See the [Ubuntu package files](https://packages.ubuntu.com/jammy/amd64/libgstreamer1.0-dev/filelist) and [dependency list](https://packages.ubuntu.com/jammy/libgstreamer-plugins-base1.0-dev).
GStreamer Check is a test dependency only. The production plugin continues to link against its existing GStreamer core libraries.

Example pixel-proof pipeline:

```sh
GST_CEF_GPU_ENABLED=1 gst-launch-1.0 -e \
  cefsrc cuda-memory=true url=file:///fixtures/pixels.html ! \
  'video/x-raw(memory:CUDAMemory),format=BGRA,width=1920,height=1080,framerate=60/1' ! \
  cefdemux name=d d.video ! cudadownload ! \
  'video/x-raw,format=BGRA' ! filesink location=/tmp/proof.bgra \
  d.audio ! fakesink
```

The download belongs only in the pixel-proof test.
For the optimized stream, connect `d.video` directly to the existing CUDA compositor.
Do not add a download, upload, or encoding stage to that path.

## Copy and lifetime contract

The EGL import uses CEF coded dimensions, visible offsets, plane stride, offset, and modifier.
BGRA maps to DRM ARGB8888. RGBA maps to DRM ABGR8888.
The shader samples semantic RGBA and writes BGRA byte order into an owned RGBA8 texture.
It preserves channel precision and premultiplied alpha without blending or sRGB conversion.

Normal view callbacks without a native popup perform exactly two consumer GPU copies:

1. Sample the imported DMA-BUF into the owned OpenGL texture.
2. Copy the mapped CUDA array into a fresh GStreamer CUDA pool buffer.

The optimized producer and consumer perform no full-frame CPU readback.
These two consumer copies do not include Chromium's internal rendering passes.
CUDA completion occurs before callback return on an owned GStreamer CUDA stream.
The consumer does not synchronize the entire compositor CUDA context.
EGL images and imported textures are destroyed inside the callback.
The consumer neither closes nor retains CEF-owned file descriptors.
The published buffer has independent metadata through the existing `gst_buffer_copy` call.
Repeated output frames share immutable memory while timestamps and audio metadata remain independent.

The demux snapshots timestamps before transferring the video buffer downstream.
The portable demux regression checks audio gaps when downstream changes its owned buffer metadata.

The consumer uses the EGL device matching the negotiated GStreamer CUDA device.
An unsupported import or CUDA failure posts a pipeline error.
CPU paint while CUDA output is selected also posts an error.
CUDA caps never carry a CPU fallback buffer.

The supported candidate also includes the native capture producer-completion patch.
It delivers the native handle only after producer GPU completion and release of the producer's write access.
The producer submits and polls GPU completion asynchronously; it does not read frame pixels into CPU memory.
Static capture does not require another browser repaint to deliver its completed frame.
Consumer `glFinish` proves completion of consumer work only and does not replace this producer contract.
The earlier candidate without producer completion depended on an unproven external synchronization assumption.
Its successful pixel tests did not establish that missing contract.
Direct EGL-to-CUDA import is not implemented in this experiment.

## Historical completion and queue002 validation

These earlier results use the task T4 with NVIDIA driver 595.71.05 and the CEF152 completion runtime through queue002.
They establish observed correctness on this configuration, not performance acceptance or support for every driver.

- CEF archive SHA256: `93ebfbf3daf822eded9f544f5da8884d673f5c49d05201f0bc29a948fcd6d178`.
- Runtime image: `gpu-transfer-candidate:152-completion-queue002`.
- Image SHA256: `2de935297286b05785f0856ed8468e468a004680f7bc1d8d83cb13120928b8cf`.
- Queue002 plugin SHA256: `a0f12379cde2c444965c85dacd0e4376a6e52ea99290654cf3d80fbc5e34f6f3`.

| Historical completion runtime check | Result |
| --- | --- |
| Deterministic 1920×1080 at 60 fps | 300 distinct measured frames; primary colors and all 256 premultiplied alpha levels pass |
| Static content | Pass after warmup without page timers or animation |
| Callback pairs while idle | All 300 measured full-frame A/B pairs match with a 1,000 µs delay |
| Pairs under bounded GPU contention | All 300 measured full-frame pairs match with a 5,000 µs delay |
| Three 1080p→720p→1080p resize cycles | Geometry, stride, alpha, pixels, EOS, and NULL cleanup pass |
| Native popups | All five positions and exact clean-view restoration pass |
| Browser features | HTML/CSS, PNG, WebGL2, Rive, video, and audio pass |
| Actual Rust overlay | Show/hide/show at 30/60 graphics fps in a 60-fps stream; decoded video, hidden silence, and restored tone pass |
| Source flush | Static changes, popup hide, and popup resize pass with zero held samples and exact restored pixels |
| Actual app lifecycle | Initial rendering, two stop/recreate cycles, refresh, URL replacement, and core-disabled renderer recovery pass |

Paired captures contain 120 warmup pairs plus 300 measured pairs, or 840 raw frames.
The stress run uses the separate bounded GPU contention tool.
These diagnostic downloads and timing delays are excluded from performance runs.
The static test also exercises the plugin's default EGL/X11 switches without extra backend flags.

Stock CEF152 still fails native allocation/import with the correct EGL/X11 flags on this T4.
The native-handle producer patch remains required; startup flags alone do not repair stock CEF152.
Stock and patched CEF152 consumer builds compiled successfully.
The existing CEF139 runtime supplies the CPU baseline.

The completion runtime and promoted queue002 consumer passed native-select, combined-feature, and actual RiveWrapper keyed lifecycle pixel reruns.
The generic wrapper fixture does not establish authored LIGR image bindings or authored hide transitions.
Direct-source HLS replacement fails on both the existing CPU baseline and the preliminary CUDA candidate.
Keyed replacement requires a later parent render in the unchanged production component.
These existing component limitations are separate from transfer correctness.

## Final clock003 validation

The final clock003 plugin uses the same CEF152 producer-completion runtime on the tested T4 and NVIDIA driver 595.71.05.
These hashes identify the final artifact; the queue002 hashes above identify historical checks only.

- Plugin SHA256: `5107772b9859d93113cd863caced696302a7cad4542ba5d2b3db78cd240290a7`.
- Runtime image SHA256: `e4cbe966d07443243c93dc67419548e25180d81255a0b49d14aad5df88886db2`.

All eight final-artifact hardware gates passed:

| Clock003 gate | Result |
| --- | --- |
| Source flush | Static update, popup hide, and popup resize; zero held samples and exact resumed pixels |
| Rust integration, 60 graphics fps | Show/hide/show; decoded video, hidden silence, and restored tone |
| Rust integration, 30 graphics fps | Same checks within a 60-fps output stream |
| Static content | Exact pixels after warmup without page timers or animation |
| Resize | Three 1080p→720p→1080p cycles; geometry, stride, pixels, alpha, EOS, and NULL |
| Native popups | Five positions, clipping, alpha, and exact clean-view restoration |
| Paired copies, zero delay | 300 measured full-frame pairs; all 256 premultiplied alpha levels and byte-identical A/B copies |
| Paired copies, 1,000 µs delay | Same 300-pair full-frame result |

Clock003 also passed the combined browser-feature fixture and actual RiveWrapper keyed lifecycle checks.
Matched CPU/CUDA full-application lifecycle reruns passed startup, stop/recreate, refresh, URL replacement, and core-disabled renderer recovery.
Five pipeline pause/resume cycles passed with monotonic video timestamps, no settled-pause samples, resumed audio, EOS, and NULL.
The earlier contention diagnostic belongs to the historical runtime; clock003 repeated the two unstressed paired-copy gates.

The final nine-run performance matrix completed three rotated runs per mode with the existing single encoder.
Its CUDA runs contained two forward counter gaps across 5,406 measured frames and no repeated counters.
The matched five-minute CPU/CUDA comparison completed with 18,003 measured frames per mode and no CUDA repeats or gaps.
These runs measured CPU, GPU load, sampled PCIe rates, frame delivery, and draw-to-encoded latency.
Startup phase and latency varied between runs. These finite samples do not establish a permanent latency ceiling.
The fifteen-minute CUDA reliability run completes with 54,002 measured frames, no repeated counters, and two forward counter gaps.
Draw-to-encoded p50/p95/maximum is 82.470/91.564/96.054 ms, with repeating latency ramps and clean EOS/NULL.
This CUDA-only run does not provide a matched resource comparison or establish a universal latency ceiling.

## Remaining coverage limits

- Check incompatible CPU/CUDA caps and retained-buffer metadata under an explicit final-artifact hardware rejection test.
- Run sustained resize, popup, reload, and stop/start loops while tracking EGL/CUDA resources, file descriptors, and pool memory.
- Run native Windows/macOS builds before claiming cross-platform build validation.

Keep host telemetry outside the measured cgroup and use identical background workloads for matched comparisons.
Sampled whole-device PCIe rates are comparative observations, not exact per-overlay byte totals or counts of device-local copies.
The harness retains frame metadata; aggregate memory growth does not establish a resource leak or leak freedom.
The source flush diagnostic does not establish demux/audio flush recovery or long-duration audiovisual pulse alignment.

Initial forced recovery exceeded its deadline while the host collected renderer core dumps.
Matched core-disabled reruns recovered on both CPU and CUDA without changing production recovery behavior.
Strict recording decode passed; encoded byte growth alone was not the acceptance criterion.
An isolated default CPU-only build against CEF139 passed before the CUDA-only scheduling addition.
Native Windows and macOS builds remain unvalidated.

The CPU baseline still ignores native popup type and placement. Its popup pixels are not a correctness reference.
The generic Rive fixture does not establish authored image bindings or authored hide transitions.

## Paired producer-readiness diagnostic

Set `cuda-diagnostic-pairs=true` explicitly to enable this test mode.
Set `cuda-pair-delay-us` from 0 through 100000; the default is 1000 microseconds.
The callback copies its borrowed CEF frame twice, with the delay between independent imports.
Both GPU copies finish before callback return.
The second copy never reads the first copy's owned output.

The queue retains at most four output frames and inserts only complete A/B pairs.
When the queue is full, the source skips the entire new callback and logs that decision at LOG level.
It never repeats a captured buffer to fill missing samples.
The output contains A/B pairs from sampled callbacks, in capture order.
Startup blank frames are excluded.
A copy failure, incomplete pair, or 30-second frame wait posts an explicit pipeline error.
The mode requires CUDA output and an even finite `num-buffers` limit.
Mid-capture caps changes fail explicitly because the checker requires a fixed frame size.

Use a separate capture for each start/stop or flush cycle.
Do not concatenate interrupted captures because an interruption can leave the final pair incomplete.
Diagnostic timing and skipped callback counts are not performance measurements.
Normal CUDA mode does not enable this queue or delay.

For 120 warmup samples and 300 measured samples, capture exactly 840 raw frames:

```sh
GST_CEF_GPU_ENABLED=1 gst-launch-1.0 -e \
  cefsrc cuda-memory=true cuda-diagnostic-pairs=true cuda-pair-delay-us=1000 \
  num-buffers=840 url=file:///fixtures/sync.html ! \
  'video/x-raw(memory:CUDAMemory),format=BGRA,width=1920,height=1080,framerate=60/1' ! \
  cefdemux name=d d.video ! cudadownload ! \
  'video/x-raw,format=BGRA' ! filesink location=/tmp/pairs.bgra \
  d.audio ! fakesink
python3 tests/gpu-transfer/check_sync_frames.py /tmp/pairs.bgra \
  --warmup 120 --frames 300 --pairs
```

The file contains packed BGRA only: callback1-A, callback1-B, callback2-A, callback2-B.
Each frame has 8,294,400 bytes. Download occurs in the test pipeline after GPU ownership transfers.
Repeat with zero delay and under GPU contention.
Matching pairs test observed stability under the sampled workloads.
They complement the producer-completion contract and do not replace it.

Browser startup now includes Chromium's standard `no-first-run` switch.
This prevents the hidden first-run EULA dialog observed with stock CEF152 on the test host.


## Native popup preservation

`OnPopupShow` and `OnPopupSize` track native widgets such as expanded HTML `<select>` controls.
CSS dialogs, HTML popovers, and custom dropdown elements remain part of normal `PET_VIEW` rendering.
`window.open` creates a separate browser and does not produce the same native-widget callback.

The existing view staging texture remains the clean base.
When a native popup paints, the consumer copies it into an owned popup-sized texture before callback return.
The consumer allocates a full-frame composite texture only when a visible popup intersects the view.
A shader combines the owned base and popup with premultiplied source-over.
The composite then follows the existing owned CUDA output copy.
Popup-only updates publish immediately, even when the main page produces no new frames.
Hide publishes the clean base immediately and releases popup resources.
Movement preserves owned popup pixels; size changes discard them until the new popup paint arrives.
Late paints from hidden or differently sized popups are ignored.
Stop rejects further popup and paint work before closing the browser.

Without a popup, the consumer allocates no popup textures and performs the original two GPU copies.
While a popup is visible, a view callback adds one full-frame GPU composition pass.
A popup callback copies the popup area, composes the full frame, and copies the result into CUDA memory.
Hide copies the retained base into CUDA memory without a new browser repaint.

CEF's default OSR screen scale is 1 for this source.
Popup rectangles retain their original view coordinates.
Clipping adjusts source offsets instead of moving the widget away from its input coordinates.
Each popup import separately validates coded size, visible size, stride, offset, format, and modifier.

Paired full-frame diagnostics reject native popups explicitly.
Their checker expects two copies of one complete source frame, rather than a composed base and popup.

Popup validation procedure (including sustained checks that remain untested):

- Open native selects at the center and each view edge with trusted CDP mouse events.
- Confirm native popup callbacks occur and the pipeline remains PLAYING.
- Verify placement, option changes, clipping, and red/blue order.
- Compare transparent and half-transparent popup pixels against premultiplied source-over expected values.
- Keep the base animated while a popup remains open. Verify both remain visible.
- Stop base animation before opening a popup. Verify popup-only changes still publish.
- Hide the popup and verify immediate clean-base restoration without ghost pixels.
- Move and resize the popup; reject old-size pixel reuse.
- Repeat open/hide, view resize, reload, and stop/start cycles while tracking GPU memory and file descriptors.
- Verify no-popup GPU copies and allocations match the existing path.
- Keep CSS show/hide and dialog checks separate from native popup checks.

An existing `window.open` lifecycle issue remains outside this change.
The shared client assigns every new browser to `src->browser` and invalidates source audio when any browser closes.
Native widget composition does not change that existing child-browser policy.

## Ordered normal delivery

Normal CUDA output retains at most two completed publications before source selection.
The source selects the oldest pending publication and keeps its last selected frame separately.
If painting pauses or runs below the stream rate, the source repeats that retained image with fresh buffer metadata.
If the queue fills, publication drops its oldest pending entry. This bounds the pending queue and its stale-frame backlog.
It does not bound total latency during downstream stalls.
The queue adds no GPU copy, encoder, or per-publication container allocation.

Popup state changes discard pending images from the preceding popup state and publish the retained clean view or new composition.
Resize clears pending references before replacing the old output pool.
Flush clears pending frames and defers popup state reconciliation until capture resumes.
A generation check rejects resume tasks invalidated by another flush or stop.
The resume task invalidates the view and any visible popup, including pages without animation.

On the tested Linux T4 runtime, two 1,802-frame encoded runs preserved every successive browser counter without repeats or skips.
A separate source flush test changed static content, hid a popup, and resized a popup during held flushes.
Each held interval delivered no samples. Resumed pixels matched their references, and EOS/NULL completed.
The flush diagnostic isolates the source; it does not establish demux or audio flush recovery.
These finite tests establish the sampled behavior, not universal frame cadence under every workload.

## Clock-scheduling experiment

Normal CUDA `create()` waits on an owned `GstClockID` before selecting the next completed publication.
It holds no source object lock during the wait. The clock has an explicit retained reference.
`get_times()` returns `GST_CLOCK_TIME_NONE` only for normal CUDA output, preventing a second BaseSrc wait or timestamp offset.
CPU output and diagnostic pairs retain their existing scheduling.

Video PTS and DTS start at pipeline running time and advance on a fixed rational frame schedule.
The schedule skips elapsed slots after a stall and marks discontinuity instead of emitting a catch-up burst.
The emitted-frame counter remains separate from schedule slots.
Pause, flush, and stop cancel the active clock wait. Resume establishes a new running-time origin.
Accepted clock changes invalidate the wait. The source also rechecks clock identity and base time before delivery.
Retry creates a new clock ID; an unscheduled ID is never reused.

Audio metadata is collected after the wait. The existing demux stamps audio packets with current pipeline running time.
The new video timestamps use that same domain. This does not change audio packet contents or demux behavior.
The latency query retains its existing one-frame minimum; it is not a measurement of browser content age.

The isolated queue003 plugin compiled successfully and matches SHA256:
`5107772b9859d93113cd863caced696302a7cad4542ba5d2b3db78cd240290a7`.
All eight portable tests pass. The clock test also passes AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled.
The helper test covers cancellation, clock replacement, base-time changes, late wakes, and 1,000 fractional-rate schedule slots.
Five actual pipeline pause/resume cycles passed with zero settled-pause samples, monotonic video PTS, audio recovery, EOS, and NULL.
Each resume delivered at most seven video frames per 100ms. Maximum observed video/audio clock lag was 1.556ms/0.096ms.
The final-artifact performance, flush, static, resize, popup, and actual overlay reruns passed as recorded above.
The fifteen-minute reliability run is recorded above; sustained resource-loop coverage remains a separate acceptance limit.
