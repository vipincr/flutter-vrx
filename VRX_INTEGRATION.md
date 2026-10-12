# flutter-vrx: integrating the app role (B2)

Status (2026-10-13): **B2 done and bench-verified on the CM4 +
CM4-DISP-BASE-2.8A.** `flutter-vrx --compositor` owns KMS and input; each
`flutter-vrx --release --app <bundle>` renders in its own process and
presents over IPC. Proven on the panel: the app's frame is scanned out
through the compositor; with two apps running, `kill -9` of the focused one
moves focus to the other, which re-presents, and the screen flips to it
while the compositor survives.

What the bench changed relative to the design below:

- **Allocation runs on the KMS node, not the render node.** vc4 scans out
  only contiguous (CMA) memory and refuses to import v3d's shmem
  (drmPrimeFDToHandle EINVAL). App mode opens the primary node as its GBM
  device, so Mesa's kmsro allocates from vc4 and renders through v3d; no DRM
  master needed. The compositor imports with drmPrimeFDToHandle into the KMS
  device (a GBM device on the KMS node returns render-side handles, which
  drmModeAddFB2 rejects with ENOENT).
- **SOCK_SEQPACKET**, so every recvmsg is one message with its fds.
- **VRX_MSG_RELEASE**: the compositor returns each buffer once it is off
  screen or superseded; the app holds at most two (GBM surfaces have four
  buffers) and skips a frame rather than starve the surface.
- **Modeset once, then page flips**, with a one-deep queue (newest wins).
- **Focus gain re-presents** the app's current front buffer: an idle app has
  no new frame to show otherwise.

The surveyed design follows as it was recorded.

## The seams (verified against this tree)

- `flutterpi_new_from_args` (flutter-pi.c:2333): libseat (optional, falls
  back cleanly) → `drmdev` (open PRIMARY node; **does not require master for
  GBM allocation**) → `window` (display geometry) → `compositor_new` →
  render surfaces → `user_input_new` → engine.
- `cmd_args.dummy_display` already exercises a non-KMS branch: fixed size,
  no page flips. App mode is that branch with real GPU rendering.
- `egl_gbm_render_surface.queue_present` (egl_gbm_render_surface.c:107) is
  the per-frame hook; `gbm_bo_get_fd(bo)` (see vk_gbm_render_surface.c:308)
  is the export idiom.
- `vrx_app_client_present()` sends the fd; the compositor owns it after.

## The delta (app mode)

1. `parse_cmd_args`: accept `--app <path>`; set `dummy_display`-like flow
   with geometry from `vrx_app_client_connect()`'s REGISTERED reply
   (connect BEFORE window creation; fail if no compositor).
2. Render surface: force the **egl_gbm** surface (GBM on the non-master
   drmdev) with a format from the REGISTERED list; disable the KMS commit —
   in `queue_present`, after eglSwapBuffers: `gbm_surface_lock_front_buffer`
   → `gbm_bo_get_fd` → `vrx_app_client_present` → `gbm_surface_release_buffer`.
   Vsync: pace on the frame scheduler's clock (v1; no compositor feedback).
3. Input: skip `user_input_new`; add the IPC fd to the sd_event loop;
   `vrx_app_client_dispatch` → map VRX_MSG_INPUT into the existing
   onFlutterPointerEvent/onFlutterTouchEvent/onFlutterKey-event callbacks
   (xkb translation via the existing keyboard.c path).
4. Focus: VRX_MSG_FOCUS(0) → stop pumping frames (suspend policy, B4
   refines).

## Compositor-side follow-ups

- BUFFER fds must be read with recvmsg (SCM_RIGHTS) in client_readable —
  the current recv() path parses the header/payload but cannot receive
  descriptors; flagged in vrx_compositor.c.
- B3: registry-driven launch/switch (a `vrx/apps/*` platform channel on the
  launcher + per-app restart policy); B4: crash containment exercise,
  background buffer release, boot-to-first-frame measurement.
