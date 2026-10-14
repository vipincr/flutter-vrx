# flutter-vrx: integrating the app role (B2, B3)

Status (2026-10-13): **B3: vrxhost launches, the compositor composites.**
The compositor role launches nothing and decides no focus. vrxhost
(vrx-linux, plan 0007 amendment C) starts each applet as the
unprivileged vrx user, connects to the compositor's socket as the one
controller (CONTROL, root only, checked by SO_PEERCRED) and names the
focused app by pid (SET_FOCUS); the compositor reports each app's arrival
and exit (APP_STATE). The socket is root:vrx 0660. REGISTERED carries the
panel's physical size, and the app's pixel ratio is (px per mm) /
VRX_DP_PER_MM, the 160 dp/inch density vrx_kit's mm metrics are written
in (1.77 on the 2.8" panel). Bench-proven on the CM4 + disp28a: a tap on
the launcher's Clock tile reached vrxhost as vrx.nav.push, vrxhost started
clock and focused it, and the screen switched; pop, home, a killed app's
restart and refocus all followed.

**B2 done and bench-verified on the CM4 +
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
- **Modeset once, then one atomic commit per vblank** for every plane, with a
  one-deep queue per source (newest wins); see "The scene" below (plan 0017 D9).
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

## The scene (plan 0017 D9)

What is on the panel is one scene: the focused app's frame on the primary
plane, the shell's frame on an overlay above it, and the dim plane (brightness
on a screen without a dimmable backlight) above both. The cursor stays on the
legacy cursor ioctls. Every change to the scene goes out as one atomic commit
flagged `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`, so the
compositor's one epoll thread never waits for a vblank. The legacy
`drmModeSetPlane`, `drmModePageFlip` and plane `drmModeObjectSetProperty` calls
are gone (each waited a vblank); the CRTC's first `drmModeSetCrtc` is the one
legacy call. Every commit sets every plane's properties, so the state is never
inherited: FB_ID, CRTC_ID, SRC_*, CRTC_*, alpha 0xffff, and rotation 180 where
the panel is mounted upside down. The dim plane carries the dim alpha; a plane
with no frame (shell hidden, dim 0) is sent with FB_ID 0.

Frames wait in slots, one set per source:

| App (primary) | Shell (overlay) | Meaning |
|---|---|---|
| `queued` | `shell_queued` | newest frame, not yet in a commit (a newer one supersedes it) |
| `pending` | `shell_pending` | carried by the commit in flight |
| `on_screen` | `shell_frame` | scanned out; freed when a commit replaces it |

Frame arrival to commit:

```
BUFFER from app or shell
  -> import dmabuf as a framebuffer
  -> shell frame, shell hidden or no modeset yet?  keep it as shell_frame, done
  -> store in queued / shell_queued (release the one it supersedes)
  -> scene_commit()
       dark                -> return (the wake sends it)
       commit in flight    -> return (the flip event sends it)
       nothing changed     -> return
       else build one atomic request, send it
         EBUSY             -> stay dirty; retry on the event, or in 4 ms
                              when nothing is known to be in flight
         refused           -> test-only commits find the dim plane or the
                              shell's plane; that plane is turned off for
                              good and the commit sent again; else the
                              frames are returned and the refusal counted
         sent              -> queued -> pending, commit_pending = true
```

Flip event to next commit:

```
page flip event (vblank time)
  -> commit_pending = false
  -> flip record: one ring entry per commit that carried a frame, headed by
     the frame that arrived last, with a `planes` column (P app, S shell,
     D dim, W wake); a commit with no frame only moves the vblank phase
  -> pending -> on_screen, shell_pending -> shell_frame (the replaced frames
     are released to their apps)
  -> tell the apps the vblank phase
  -> scene_commit()   (anything that went dirty meanwhile, as one commit)
  -> waking and nothing in flight?  light the screen
```

Screen power: off is a waiting atomic commit setting the CRTC's `ACTIVE` to 0
(with `ALLOW_MODESET`, after the light goes to 0); a commit still in flight
finishes inside it. On is the scene's next commit with `ACTIVE` 1, the newest
frames and the dim alpha in one request, after which the light comes up on the
flip event. Frames that arrive while the screen is off wait in `queued`.

The pointer grab: a button press records its owner (the shell inside its
region, else the focused app) and every pointer event goes there until the
last button is released, as a touch contact does; hover keeps the position
rule (`input_target`).

## Compositor-side follow-ups

- BUFFER fds must be read with recvmsg (SCM_RIGHTS) in client_readable —
  the current recv() path parses the header/payload but cannot receive
  descriptors; flagged in vrx_compositor.c.
- B3 landed as above: launch, restart policy and focus live in vrxhost;
  the launcher talks to vrxhost over its intent socket (vrx_kit's
  VrxHost), not a platform channel.
- B4: suspend unfocused apps (FOCUS(0) is already sent), background
  buffer release, boot-to-first-frame measurement.
