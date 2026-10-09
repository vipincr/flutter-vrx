# flutter-vrx: integrating the app role (B2 completion design)

Status: compositor role (`--compositor`) and the IPC client
(`vrx_app_client.c`) compile cross-clean. What remains is wiring the client
into `flutterpi_app_main` so `--app <bundle>` runs the stock engine path but
presents over IPC instead of KMS. This file records the surveyed design so it
is executed, not re-derived.

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
