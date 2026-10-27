@import CompositorServices;
@import Darwin;
@import ObjectiveC;
@import UniformTypeIdentifiers;
//#include <Metal/Metal.h>
//#include <simd/matrix_types.h>
//#include <CompositorServices/drawable.h>

#include "openxr-Bridging-Header.h"
#include <stdatomic.h>
#include <pthread.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                           \
  __attribute__((used)) static struct {                                                   \
    const void* replacement;                                                              \
    const void* replacee;                                                                 \
  } _interpose_##_replacee __attribute__((section("__DATA,__interpose,interposing"))) = { \
      (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee}

// 6.5cm translation on x
static simd_float4x4 gIdentityMat = {.columns = {
                                            {1, 0, 0, 0},
                                            {0, 1, 0, 0},
                                            {0, 0, 1, 0},
                                            {0, 0, 0, 1},
                                        }};
static simd_float4x4 gWorldMat = {.columns = {
                                            {1, 0, 0, 0},
                                            {0, 1, 0, 0},
                                            {0, 0, 1, 0},
                                            {0, 2.0, 0, 1},
                                        }};
static simd_float4x4 gNewWorldMat = {.columns = {
                                            {1, 0, 0, 0},
                                            {0, 1, 0, 0},
                                            {0, 0, 1, 0},
                                            {0, 2.0, 0, 1},
                                        }};

static simd_float4x4 left_controller_pose[3];
static simd_float4x4 right_controller_pose[3];

simd_float4x4 view_mat_l[3];
simd_float4x4 view_mat_r[3];
openxr_headset_data xr_data[3];

static simd_float4 left_eye_pos[3];
static simd_float4 left_eye_quat[3];
static simd_float4 left_eye_zbasis[3];
static simd_float4 right_eye_pos[3];
static simd_float4 right_eye_quat[3];

// cp_drawable_get_view
struct cp_view {
  simd_float4x4 transform;     // 0x0
  simd_float4 tangents;        // 0x40
  uint32_t unknown_1[4];  // 0x50 (0x110 - 0x50)/4
  uint32_t unknown_2[4];  // 0x60 (0x110 - 0x50)/4
  float unknown_3[4];  // 0x70 (0x110 - 0x50)/4
  float unknown_4[3];  // 0x80 (0x110 - 0x50)/4
  float height_maybe;
  uint32_t unknown[0x20];  // 0x90 (0x110 - 0x90)/4
};
static_assert(sizeof(struct cp_view) == 0x110, "cp_view size is wrong");

// cp_view_texture_map_get_texture_index
struct cp_view_texture_map {
  size_t texture_index;  // 0x0
  size_t slice_index;    // 0x8
  MTLViewport viewport;  // 0x10
};

struct RSSimulatedHeadsetPose {
  simd_float4 position;
  simd_float4 rotation;
};

/*
typedef struct cp_time {
  uint64_t cp_mach_abs_time;
} cp_time;
*/

typedef struct cp_frame_timing
{
  uint64_t unk_0;
  uint64_t unk_8;
  uint64_t unk_10;
  uint64_t unk_18;
  uint64_t unk_20;
  uint64_t target_vbl;
  uint64_t unk_30;
  uint64_t time_base_idk;
  uint64_t unk_40;
  uint64_t unk_48;
  uint64_t idk_offset;
  uint64_t optimal_input_time;
  uint64_t rendering_deadline;
  uint64_t presentation_time;
  uint64_t unk_70;
  uint64_t unk_78;
  uint64_t unk_80;
} cp_frame_timing;

// Borrow native pool textures while encoding their drawable; the bridge owns
// separate capture textures and does not extend native wrapper lifetimes.
static __unsafe_unretained id<MTLTexture> gOrigTextureList[3] = {nil,nil,nil};

// TODO(zhuowei): multiple screenshots in flight
static cp_drawable_t gHookedDrawable[3] = {nil,nil,nil};;

static id<MTLTexture> gHookedRightTexture[3] = {nil,nil,nil};
static id<MTLTexture> gHookedRightDepthTexture[3] = {nil,nil,nil};
static id<MTLTexture> gHookedLeftTexture[3] = {nil,nil,nil};
static id<MTLTexture> gHookedLeftDepthTexture[3] = {nil,nil,nil};
static id<MTLTexture> gHookedRightTextureCopy[3] = {nil,nil,nil};
static id<MTLTexture> gHookedLeftTextureCopy[3] = {nil,nil,nil};

static __unsafe_unretained id<MTLTexture> gHookedSimulatorPreviewTexture[3] = {nil,nil,nil};
static id<MTLSharedEvent> copyDoneL[3] = {nil,nil,nil};
static int poseIdxs[3] = {0,0,0};
static int native_capture_slot = 0;
static atomic_int capture_start_state = 0; // idle, starting, ready, failed
static atomic_bool capture_pose_pending[3] = {false, false, false};
static pthread_mutex_t capture_pose_mutex = PTHREAD_MUTEX_INITIALIZER;

#define NUM_VIEWS (2)

static bool replace_simulator_eyes(cp_drawable_t drawable) {
  const char *enabled = getenv("XRHAX_REPLACE_SIMULATOR_EYES");
  // Never invent a second private view in a native one-view drawable.
  return enabled && !strcmp(enabled, "1") && cp_drawable_get_view_count(drawable) >= 2;
}

uint64_t openxr_texture_pixel_format(MTLTexture_id texture) {
  return texture.pixelFormat;
}

@interface RSSimulatedHeadset
- (void)getEyePose:(struct RSSimulatedHeadsetPose*)pose forEye:(int)forEye;
- (void)setEyePose:(struct RSSimulatedHeadsetPose)pose forEye:(int)forEye;
- (void)setHMDPose:(struct RSSimulatedHeadsetPose)pose;
@end

RSSimulatedHeadset* gHookedSimulatedHeadset = nil;
SEL gHookedSimulatedHeadset_sel;
static void (*real_RSSimulatedHeadset_setHMDPose)(RSSimulatedHeadset* self, SEL sel,
                                                  struct RSSimulatedHeadsetPose pose);

static float CalculateFovY(float fovX, float aspect) {
  // https://cs.android.com/android/platform/superproject/+/master:external/exoplayer/tree/library/ui/src/main/java/com/google/android/exoplayer2/ui/spherical/SphericalGLSurfaceView.java;l=328;drc=2c30c028bf6b958edf635b3e4e1079e64737250a
  float halfFovX = fovX / 2;
  float tanY = tanf(halfFovX) / aspect;
  float halfFovY = atanf(tanY);
  return halfFovY * 2;
}

static id<MTLTexture> MakeOurTextureBasedOnTheirTexture(id<MTLDevice> device,
                                                        id<MTLTexture> originalTexture) {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:originalTexture.pixelFormat
                                                         width:originalTexture.width
                                                        height:originalTexture.height
                                                     mipmapped:false];
  descriptor.resourceOptions = originalTexture.resourceOptions;
  descriptor.usage = originalTexture.usage;
  descriptor.textureType = originalTexture.textureType;
  descriptor.arrayLength = originalTexture.arrayLength;
  descriptor.sampleCount = originalTexture.sampleCount;
  return [device newTextureWithDescriptor:descriptor];
}

int _os_feature_enabled_impl(const char* a, const char* b);
static int hook__os_feature_enabled_impl(const char* a, const char* b)
{
  int ret = _os_feature_enabled_impl(a,b);
  if (!strcmp(a, "RealitySimulation") && !strcmp(b, "OverlapRenderAndSimulation")) {
    //ret = 1;
  }
  //printf("Feature: %s, %s -> %x\n", a, b, ret);
  return ret;
}
DYLD_INTERPOSE(hook__os_feature_enabled_impl, _os_feature_enabled_impl);

static int num_frames_done = 0;

#if 1
static cp_frame_timing* hook_cp_frame_predict_timing(void* a)
{
  cp_frame_timing* ret = cp_frame_predict_timing(a);

  // The private timing layout below is from visionOS 1.0 beta. Do not write
  // those offsets on newer runtimes; let the compositor own its frame pacing.
  if (!ret || !getenv("XRHAX_LEGACY_FRAME_TIMING")) return ret;

  //ret->time_base_idk -= 199998;
  ret->rendering_deadline = 0;
  ret->presentation_time = 0;
  ret->idk_offset = 199998;//149998/2;
  //printf("%llx %llx\n", ret->optimal_input_time, ret->idk_offset);

  return ret;
}
DYLD_INTERPOSE(hook_cp_frame_predict_timing, cp_frame_predict_timing);

static struct cp_time hook_cp_frame_timing_get_presentation_time(void* a)
{
  // +0x68 double 0.008333 (120Hz)
  // +0x28 frame idx
  //printf("testing %f\n", *(double*)((intptr_t)a + 0x68));
  //printf("testing %d\n", *(int*)((intptr_t)a + 0x28));

  //struct cp_time val = {mach_absolute_time()};
  //return val;

  return cp_frame_timing_get_presentation_time(a);
}
DYLD_INTERPOSE(hook_cp_frame_timing_get_presentation_time, cp_frame_timing_get_presentation_time);

//extern  struct cp_time hook_cp_frame_timing_get_optimal_input_time(void* a);
static struct cp_time hook_cp_frame_timing_get_optimal_input_time(void* a)
{
  /*struct cp_time val = cp_frame_timing_get_rendering_deadline(a);
  val.cp_mach_abs_time -= 40000;
  return val;*/
  //struct cp_time val = {mach_absolute_time()};
  //return val;
  return cp_frame_timing_get_optimal_input_time(a);
}
DYLD_INTERPOSE(hook_cp_frame_timing_get_optimal_input_time, cp_frame_timing_get_optimal_input_time);

static struct cp_time hook_cp_frame_timing_get_rendering_deadline(void* a)
{
  //struct cp_time val = {mach_absolute_time() + 80000};
  //return val;

  return cp_frame_timing_get_rendering_deadline(a);
}
DYLD_INTERPOSE(hook_cp_frame_timing_get_rendering_deadline, cp_frame_timing_get_rendering_deadline);

extern int cp_frame_timing_get_frame_repeat_count(void* a);
static int hook_cp_frame_timing_get_frame_repeat_count(void* a)
{
  return cp_frame_timing_get_frame_repeat_count(a);
}
DYLD_INTERPOSE(hook_cp_frame_timing_get_frame_repeat_count, cp_frame_timing_get_frame_repeat_count);

#endif

#if 0
void RERenderManagerWaitForFramePacing(void* ctx);
void hook_RERenderManagerWaitForFramePacing(void* ctx)
{

}
DYLD_INTERPOSE(hook_RERenderManagerWaitForFramePacing, RERenderManagerWaitForFramePacing);
#endif


int num_buffers_collected()
{
  int ret = 0;
  for (int i = 0; i < 3; i++)
  {
    if (gHookedLeftTextureCopy[i] && gHookedRightTextureCopy[i]) {
      ret++;
    }
  }
  return ret;
}

int which_buffer_is_this(id<MTLTexture> originalTexture)
{
  if (!originalTexture) return -1;
  if (!getenv("XRHAX_REPLACE_SIMULATOR_EYES")) return native_capture_slot;

  for (int i = 0; i < 3; i++)
  {
    if (gOrigTextureList[i] == originalTexture) {
      return i;
    }
  }

  for (int i = 0; i < 3; i++)
  {
    if (gOrigTextureList[i] == nil) {
      gOrigTextureList[i] = originalTexture;
      return i;
    }
  }

  return -1;
}

static atomic_int last_which = -1;
static atomic_int pulled_which = -1;
static void pull_openxr_data_impl()
{
  int which_guess = (last_which + 1) % 3;

  if (pulled_which == which_guess) {
    return;
  }

  // We do this first, because query_drawable might stall...?
  memset(&xr_data[which_guess], 0, sizeof(xr_data[which_guess]));
  if (num_buffers_collected() >= 3)
  {
    int idle = 0;
    if (atomic_compare_exchange_strong(&capture_start_state, &idle, 1)) {
      // Pairing can take tens of seconds. Never block the system compositor
      // during drawable acquisition or a simulator service's launch sequence.
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int result = openxr_set_textures(gHookedLeftTextureCopy, gHookedRightTextureCopy,
            gHookedRightTextureCopy[0].width, gHookedRightTextureCopy[0].height);
        NSLog(@"[Sim2OpenXR] Background OpenXR startup %@.", result ? @"failed" : @"ready");
        atomic_store(&capture_start_state, result ? 3 : 2);
      });
    }
    if (atomic_load(&capture_start_state) != 2) return;

    // this will wait on headset data, this pose MUST be synced with the frame send w/ xrEndFrame
    int pose_index = openxr_headset_get_data(&xr_data[which_guess], which_guess);
    if (pose_index < 0) return;
    poseIdxs[which_guess] = pose_index;

    pulled_which = which_guess;
  }
  //printf("%u pull_openxr_data(%u)\n", which_guess, pulled_which == which_guess);

  openxr_headset_data* pData = &xr_data[which_guess];

  // Apple
  // X is -left/+right
  // Y is +up/-down
  // Z is -forward/+back
  left_eye_pos[which_guess][0] = pData->l_x;
  left_eye_pos[which_guess][1] = pData->l_y;
  left_eye_pos[which_guess][2] = pData->l_z;
  left_eye_pos[which_guess][3] = 0.0;

  left_eye_quat[which_guess][0] = pData->l_qx;
  left_eye_quat[which_guess][1] = pData->l_qy;
  left_eye_quat[which_guess][2] = pData->l_qz;
  left_eye_quat[which_guess][3] = pData->l_qw;

  right_eye_pos[which_guess][0] = pData->r_x;
  right_eye_pos[which_guess][1] = pData->r_y;
  right_eye_pos[which_guess][2] = pData->r_z;
  right_eye_pos[which_guess][3] = 0.0;

  right_eye_quat[which_guess][0] = pData->r_qx;
  right_eye_quat[which_guess][1] = pData->r_qy;
  right_eye_quat[which_guess][2] = pData->r_qz;
  right_eye_quat[which_guess][3] = pData->r_qw;

  if (pData->view_l)
  {
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        view_mat_l[which_guess].columns[i][j] = pData->view_l[(i*4)+j];
      }
    }
  }

  if (pData->view_r)
  {
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        view_mat_r[which_guess].columns[i][j] = pData->view_r_rel[(i*4)+j];
      }
    }
  }

  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 4; j++)
    {
      left_controller_pose[which_guess].columns[i][j] = pData->l_controller[(i*4)+j];
      right_controller_pose[which_guess].columns[i][j] = pData->r_controller[(i*4)+j];
    }
  }

  left_eye_zbasis[which_guess][0] = view_mat_l[which_guess].columns[2][0];
  left_eye_zbasis[which_guess][1] = view_mat_l[which_guess].columns[2][1];
  left_eye_zbasis[which_guess][2] = view_mat_l[which_guess].columns[2][2];
  left_eye_zbasis[which_guess][3] = view_mat_l[which_guess].columns[2][3];

  view_mat_l[which_guess] = gIdentityMat;
  gNewWorldMat = view_mat_l[which_guess];

  if (gHookedSimulatedHeadset) {
    struct RSSimulatedHeadsetPose pose;
    pose.position = left_eye_pos[which_guess];
    pose.rotation = left_eye_quat[which_guess];

    //real_RSSimulatedHeadset_setHMDPose(gHookedSimulatedHeadset, gHookedSimulatedHeadset_sel, pose);
  }
  if (pulled_which == which_guess) atomic_store(&capture_pose_pending[which_guess], true);
}

void pull_openxr_data()
{
  if (pthread_mutex_trylock(&capture_pose_mutex)) return;
  pull_openxr_data_impl();
  pthread_mutex_unlock(&capture_pose_mutex);
}

static cp_drawable_t prepare_drawable(cp_drawable_t retval) {
  if (getenv("XRHAX_CAPTURE_PAUSED")) return retval;
  if (!retval) return retval;
  num_frames_done++;

  int which = which_buffer_is_this(cp_drawable_get_color_texture(retval, 0));
  // A paused layer can have no texture, and capture/resized drawables can
  // exceed the legacy triple buffer. Never use -1 as an array index.
  if (which < 0) return retval;
  if (num_frames_done <= 3) {
    NSLog(@"[Sim2OpenXR] Drawable %d: slot=%d buffers=%d size=%lux%lu", num_frames_done, which,
          num_buffers_collected(), (unsigned long)cp_drawable_get_color_texture(retval, 0).width,
          (unsigned long)cp_drawable_get_color_texture(retval, 0).height);
  }
  openxr_headset_data* pData = &xr_data[which];
  if (!replace_simulator_eyes(retval)) {
    id<MTLTexture> source = cp_drawable_get_color_texture(retval, 0);
    if (!gHookedLeftTextureCopy[0]) {
      // The simulator may recycle a single array texture. Own a separate 2D
      // capture ring rather than waiting for three native texture identities.
      MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
          width:source.width height:source.height mipmapped:NO];
      desc.storageMode = MTLStorageModePrivate;
      desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
      if (cp_drawable_get_view_count(retval) > 0) {
        simd_float4 tangents = cp_view_get_tangents(cp_drawable_get_view(retval, 0));
        openxr_set_mono_tangents(tangents.x, tangents.y, tangents.z, tangents.w);
        float angles[4];
        if (openxr_get_mono_fov(angles)) {
          NSLog(@"[Sim2OpenXR] Native mono FOV radians L/R/U/D: %.3f %.3f %.3f %.3f",
              angles[0], angles[1], angles[2], angles[3]);
        } else {
          NSLog(@"[Sim2OpenXR] Native mono projection unavailable: invalid tangent metadata.");
        }
      }
      for (int slot = 0; slot < 3; ++slot) {
        gHookedLeftTextureCopy[slot] = [source.device newTextureWithDescriptor:desc];
        gHookedRightTextureCopy[slot] = [source.device newTextureWithDescriptor:desc];
      }
      NSLog(@"[Sim2OpenXR] Native capture ring ready: %d slots, %lux%lu, mono image to both eyes.",
          num_buffers_collected(), (unsigned long)source.width, (unsigned long)source.height);
    }
    gHookedDrawable[which] = retval;
    gHookedSimulatorPreviewTexture[which] = source;
    // Modern RealitySimulation can bypass the public color-texture getter and
    // setHMDPose only when input changes. Initialize at drawable acquisition.
    pull_openxr_data();
    return retval;
  }
  //printf("%u hook_cp_frame_query_drawable (idx %u)\n", which, poseIdxs[which]);
  if (!gHookedRightTexture[which]) {
    // only make this once
    id<MTLTexture> originalTexture = cp_drawable_get_color_texture(retval, 0);
    id<MTLTexture> originalDepthTexture = cp_drawable_get_depth_texture(retval, 0);
    id<MTLDevice> metalDevice = originalTexture.device;//MTLCreateSystemDefaultDevice();
    
    gHookedRightTexture[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalTexture);
    gHookedRightDepthTexture[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalDepthTexture);
    gHookedLeftTexture[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalTexture);
    gHookedLeftDepthTexture[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalDepthTexture);

    gHookedRightTextureCopy[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalTexture);
    gHookedLeftTextureCopy[which] = MakeOurTextureBasedOnTheirTexture(metalDevice, originalTexture);
    copyDoneL[which] = [metalDevice newSharedEvent];
  }

  gHookedDrawable[which] = retval;
  gHookedSimulatorPreviewTexture[which] = cp_drawable_get_color_texture(retval, 0);

  if (!replace_simulator_eyes(retval)) {
    static dispatch_once_t native_capture_log;
    dispatch_once(&native_capture_log, ^{
      NSLog(@"[Sim2OpenXR] Native capture mode: preserving simulator views/textures; duplicating its image for both Quest eyes.");
    });
    return retval;
  }

  cp_view_t simView = cp_drawable_get_view(retval, 0);
  cp_view_t rightView = cp_drawable_get_view(retval, 1);
  // Preserve the runtime's per-view private state. Copying the entire beta-era
  // cp_view layout overwrites fields that modern compositors own.

  cp_view_get_view_texture_map(rightView)->texture_index = 1;
  cp_view_get_view_texture_map(simView)->texture_index = 0;

  float fovAngleLeft_l   = -42 * M_PI / 180;
  float fovAngleRight_l  =  40 * M_PI / 180;
  float fovAngleTop_l    =  54 * M_PI / 180;
  float fovAngleBottom_l = -54 * M_PI / 180;

  float fovAngleLeft_r   = -40 * M_PI / 180;
  float fovAngleRight_r  =  42 * M_PI / 180;
  float fovAngleTop_r    =  54 * M_PI / 180;
  float fovAngleBottom_r = -54 * M_PI / 180;

  // https://git.sr.ht/~sircmpwn/wxrc/tree/master/item/src/xrutil.c#L313
  simd_float4 tangents_l = simd_make_float4(tanf(-fovAngleLeft_l), tanf(fovAngleRight_l),
                                          tanf(fovAngleTop_l), tanf(-fovAngleBottom_l));
  simd_float4 tangents_r = simd_make_float4(tanf(-fovAngleLeft_r), tanf(fovAngleRight_r),
                                          tanf(fovAngleTop_r), tanf(-fovAngleBottom_r));

  simView->tangents = tangents_l;
  rightView->tangents = tangents_r;

  if (pData->tangents_l)
  {
    simd_float4 tangents_l = simd_make_float4(tanf(-pData->tangents_l[0]), tanf(pData->tangents_l[1]),
                                              tanf(pData->tangents_l[2]), tanf(-pData->tangents_l[3]));
    simView->tangents = tangents_l;
  }
  
  if (pData->tangents_r)
  {
    simd_float4 tangents_r = simd_make_float4(tanf(-pData->tangents_r[0]), tanf(pData->tangents_r[1]),
                                              tanf(pData->tangents_r[2]), tanf(-pData->tangents_r[3]));
    rightView->tangents = tangents_r;
  }

  //simView->transform = view_mat_l;
  // Keep the simulator's valid eye transforms until the first tracked pose.
  // Zero-initialized matrices are singular and invalid for the modern renderer.
  if (pData->view_r) {
    rightView->transform = view_mat_r[which];
    simView->transform = gIdentityMat;
  }

  return retval;
}

static cp_drawable_t hook_cp_frame_query_drawable(cp_frame_t frame) {
  return prepare_drawable(cp_frame_query_drawable(frame));
}
DYLD_INTERPOSE(hook_cp_frame_query_drawable, cp_frame_query_drawable);

#if __VISION_OS_VERSION_MAX_ALLOWED >= 260000
// visionOS 26 introduced a drawable array for display and capture targets.
// Keep the legacy entry point as well: existing compositor clients still use it.
static cp_drawable_array_t hook_cp_frame_query_drawables(cp_frame_t frame) {
  cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
  for (size_t index = 0; index < cp_drawable_array_get_count(drawables); ++index) {
    cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, index);
    if (cp_drawable_get_target(drawable) == cp_drawable_target_built_in) {
      prepare_drawable(drawable);
    }
  }
  return drawables;
}
DYLD_INTERPOSE(hook_cp_frame_query_drawables, cp_frame_query_drawables);
#endif

#if 0
extern void* RERenderFrameSettingsAddGpuWaitEvent(void* a, void* b, void* c);
void* hook_RERenderFrameSettingsAddGpuWaitEvent(void* a, void* b, void* c)
{
  return NULL;
}
DYLD_INTERPOSE(hook_RERenderFrameSettingsAddGpuWaitEvent, RERenderFrameSettingsAddGpuWaitEvent);
#endif

extern void cp_drawable_present(cp_drawable_t drawable);
static int buffer_delay = 0;
static void hook_cp_drawable_encode_present(cp_drawable_t drawable,
                                            id<MTLCommandBuffer> command_buffer) {
  if (getenv("XRHAX_CAPTURE_PAUSED")) {
    cp_drawable_encode_present(drawable, command_buffer);
    return;
  }
  static dispatch_once_t logged;
  dispatch_once(&logged, ^{ NSLog(@"[Sim2OpenXR] Drawable presentation callback active."); });
  // Modern compositor clients may present a different wrapper from the one
  // observed at acquisition. Capture the current drawable at this boundary.
  prepare_drawable(drawable);
  int which = which_buffer_is_this(cp_drawable_get_color_texture(drawable, 0));
  if (which < 0) {
    cp_drawable_encode_present(drawable, command_buffer);
    return;
  }
  last_which = which;
  if (gHookedSimulatorPreviewTexture[which] && pulled_which >= 0 &&
      atomic_load(&capture_start_state) == 2 && num_buffers_collected() >= 3 &&
      (!replace_simulator_eyes(drawable) || buffer_delay >= 1) &&
      atomic_exchange(&capture_pose_pending[which], false)) {
    int frame_pose_index = poseIdxs[which];
    openxr_spawn_renderframe(which, frame_pose_index);
    static dispatch_once_t first_copy;
    dispatch_once(&first_copy, ^{ NSLog(@"[Sim2OpenXR] First video copy queued from the presented drawable."); });

    BOOL stereo = replace_simulator_eyes(drawable);
    if (stereo) {
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      [blit copyFromTexture:gHookedLeftTexture[which] toTexture:gHookedSimulatorPreviewTexture[which]];
      [blit endEncoding];
    }

    id<MTLBlitCommandEncoder> blitL = [command_buffer blitCommandEncoder];
    [blitL copyFromTexture:stereo ? gHookedLeftTexture[which] : gHookedSimulatorPreviewTexture[which]
        sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(gHookedLeftTextureCopy[which].width, gHookedLeftTextureCopy[which].height, 1)
        toTexture:gHookedLeftTextureCopy[which] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blitL endEncoding];

    id<MTLBlitCommandEncoder> blitR = [command_buffer blitCommandEncoder];
    [blitR copyFromTexture:stereo ? gHookedRightTexture[which] : gHookedSimulatorPreviewTexture[which]
        sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(gHookedRightTextureCopy[which].width, gHookedRightTextureCopy[which].height, 1)
        toTexture:gHookedRightTextureCopy[which] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blitR endEncoding];

    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      static dispatch_once_t first_completion;
      dispatch_once(&first_completion, ^{
        NSLog(@"[Sim2OpenXR] First video copy GPU status=%lu error=%@", (unsigned long)buffer.status, buffer.error);
      });
      openxr_complete_renderframe(which, frame_pose_index);
    }];
  }

  if (num_buffers_collected() >= 3 )
    buffer_delay = 1;

  cp_drawable_encode_present(drawable, command_buffer);
  if (!replace_simulator_eyes(drawable)) {
    gHookedDrawable[which] = NULL;
    gHookedSimulatorPreviewTexture[which] = nil;
    native_capture_slot = (which + 1) % 3;
  }

  //[command_buffer commit];
  //cp_drawable_present(drawable);
}

DYLD_INTERPOSE(hook_cp_drawable_encode_present, cp_drawable_encode_present);

#if 0
int RCPHIDEventGetSelectionRay(void* ctx, struct RSSimulatedHeadsetPose* pose);
int hook_RCPHIDEventGetSelectionRay(void* ctx, struct RSSimulatedHeadsetPose* pose) {
  int ret = RCPHIDEventGetSelectionRay(ctx, pose);

#if 0
  pose->position = right_controller_pose.columns[3];
  pose->rotation[0] = -right_controller_pose.columns[2][0];
  pose->rotation[1] = -right_controller_pose.columns[2][1];
  pose->rotation[2] = -right_controller_pose.columns[2][2];

//#ifdef EYE_CURSOR
  pose->position = left_eye_pos;
  //pose->rotation = left_eye_quat;
  pose->rotation[0] = -left_eye_zbasis[0];
  pose->rotation[1] = -left_eye_zbasis[1];
  pose->rotation[2] = -left_eye_zbasis[2];
//#endif
#endif

  //printf("selection ray %u, %f %f %f %f, %f %f %f %f\n", ret, pose->position[0], pose->position[1], pose->position[2], pose->position[3], pose->rotation[0], pose->rotation[1], pose->rotation[2], pose->rotation[3]);
  return ret;
}
DYLD_INTERPOSE(hook_RCPHIDEventGetSelectionRay, RCPHIDEventGetSelectionRay);
#endif

static size_t hook_cp_drawable_get_view_count(cp_drawable_t drawable) { return cp_drawable_get_view_count(drawable); }
DYLD_INTERPOSE(hook_cp_drawable_get_view_count, cp_drawable_get_view_count);

static size_t hook_cp_drawable_get_texture_count(cp_drawable_t drawable) { return cp_drawable_get_texture_count(drawable); }
DYLD_INTERPOSE(hook_cp_drawable_get_texture_count, cp_drawable_get_texture_count);

static id<MTLTexture> hook_cp_drawable_get_color_texture(cp_drawable_t drawable, size_t index) {
  if (getenv("XRHAX_CAPTURE_PAUSED")) return cp_drawable_get_color_texture(drawable, index);
  // The modern system renderer acquires drawables through an internal path,
  // bypassing the public frame-query functions. Prepare at texture access too.
  if (index == 0) {
    prepare_drawable(drawable);
    pull_openxr_data();
  }
  if (!replace_simulator_eyes(drawable)) return cp_drawable_get_color_texture(drawable, index);
  int which = which_buffer_is_this(cp_drawable_get_color_texture(drawable, 0));
  if (which < 0) return cp_drawable_get_color_texture(drawable, index);
  //printf("hook_cp_drawable_get_color_texture(%u) %u\n", index, which);
  if (index == 1) {
    return gHookedRightTexture[which];
  }
  else if (index == 2 || index == 0) {
    return gHookedLeftTexture[which];
  }
  return cp_drawable_get_color_texture(drawable, 0);
}

DYLD_INTERPOSE(hook_cp_drawable_get_color_texture, cp_drawable_get_color_texture);

static id<MTLTexture> hook_cp_drawable_get_depth_texture(cp_drawable_t drawable, size_t index) {
  if (!replace_simulator_eyes(drawable)) return cp_drawable_get_depth_texture(drawable, index);
  int which = which_buffer_is_this(cp_drawable_get_color_texture(drawable, 0));
  if (which < 0) return cp_drawable_get_depth_texture(drawable, index);
  if (!gHookedLeftDepthTexture[which]) prepare_drawable(drawable);
  //printf("hook_cp_drawable_get_depth_texture(%u) %u\n", index, which);
  if (index == 1) {
    return gHookedRightDepthTexture[which];
  }
  else if (index == 2 || index == 0) {
    return gHookedLeftDepthTexture[which];
  }
  return cp_drawable_get_depth_texture(drawable, 0);
}

DYLD_INTERPOSE(hook_cp_drawable_get_depth_texture, cp_drawable_get_depth_texture);

// Compatibility for legacy callers requesting the former preview texture.
// The current renderer exposes only the two eye textures.
static id<MTLRasterizationRateMap> hook_cp_drawable_get_rasterization_rate_map(cp_drawable_t drawable, size_t index) {
  static dispatch_once_t logged;
  dispatch_once(&logged, ^{
    NSLog(@"[Sim2OpenXR] Rasterization access: index=%lu format=%lu type=%lu views=%lu textures=%lu",
          (unsigned long)index, (unsigned long)cp_drawable_get_color_texture(drawable, 0).pixelFormat,
          (unsigned long)cp_drawable_get_color_texture(drawable, 0).textureType,
          (unsigned long)cp_drawable_get_view_count(drawable), (unsigned long)cp_drawable_get_texture_count(drawable));
  });
  prepare_drawable(drawable);
  if (index == NUM_VIEWS) index = 0;
  return cp_drawable_get_rasterization_rate_map(drawable, index);
}
DYLD_INTERPOSE(hook_cp_drawable_get_rasterization_rate_map, cp_drawable_get_rasterization_rate_map);

static id<MTLRasterizationRateMap> hook_cp_drawable_get_flipped_rasterization_rate_map(cp_drawable_t drawable, size_t index) {
  if (index == NUM_VIEWS) index = 0;
  return cp_drawable_get_flipped_rasterization_rate_map(drawable, index);
}
DYLD_INTERPOSE(hook_cp_drawable_get_flipped_rasterization_rate_map, cp_drawable_get_flipped_rasterization_rate_map);

size_t cp_layer_renderer_properties_get_view_count(cp_layer_renderer_properties_t properties);

static size_t hook_cp_layer_renderer_properties_get_view_count(cp_layer_renderer_properties_t properties) {
  return cp_layer_renderer_properties_get_view_count(properties);
}

DYLD_INTERPOSE(hook_cp_layer_renderer_properties_get_view_count, cp_layer_renderer_properties_get_view_count);


#if 0
void RERenderFrameSettingsSetTotalTime(void* re, float time);
static void hook_RERenderFrameSettingsSetTotalTime(void* re, float time) {
  //printf("_RERenderFrameSettingsSetTotalTime %f\n", time);
  RERenderFrameSettingsSetTotalTime(re, 0.0); //time - 0.03125 + 0.008
}

DYLD_INTERPOSE(hook_RERenderFrameSettingsSetTotalTime, RERenderFrameSettingsSetTotalTime);
#endif

//RSXRRenderLoop::currentFrequency

#if 0
static double (*real_RSUserSettings_worstAllowedFrameTime)(void* self, double val);
static double hook_RSUserSettings_worstAllowedFrameTime(void* self, double val) {
  double ret = real_RSUserSettings_worstAllowedFrameTime(self, val);
  //printf("worstAllowedFrameTime %f %f\n", val, ret);
  return ret;
}
#endif

#if 1
static int (*real_RSXRRenderLoop_currentFrequency)(void* self);
static int hook_RSXRRenderLoop_currentFrequency(void* self) {
  int ret = real_RSXRRenderLoop_currentFrequency(self);
  //printf("frequency at %d\n", ret);

#if 0  
  //printf("%f\n", *(double *)(self + 0x188));
  //*(int *)(self + 0x140) = 2; //overcomitted
  //*(int *)(self + 0x144) = 3;
  //*(int *)(self + 0x148) = 2;
  //*(int *)(self + 0x13C) = 3;
  *(double *)(self + 0x1a0) = 0.004; //autoFPSWorstAllowedFrameTime
  *(double *)(self + 0x1a8) = 0.004; //baseFrameDurationMAT 
  *(double *)(self + 0x1B0) = 0.004; //baseFrameDuration 
  *(double *)(self + 0x1B8) = 0.004;//399999.0 / 2.0; // compositorTargetFrameDurationMAT
  //*(double *)(self + 0x1C0) = //compositorTargetFrameDuration
  //*(double *)(self + 0x1C8) = //allowedHeadStart
  //printf("idk %f\n", *(double *)(self + 0x1B8));
  *(int*)((intptr_t)self + 0x20C) = 240;

  *(double *)(self + 0x150) = -0.000050;
  *(double *)(self + 0x158) = -0.000050;
  *(double *)(self + 0x160) = 0.000000;

  //*(double *)(self + 0x1A0) = 0.000000050;

  /*
  *(double *)(self + 0x150) = 0.000050;
  *(double *)(self + 0x158) = 0.000050;
  *(double *)(self + 0x160) = 2.000000;
  */

  //printf("test %f %f %f\n", *(double *)(self + 0x150), *(double *)(self + 0x158), *(double *)(self + 0x160));
#endif
  return ret;
}
#endif

static void (*real_RSSimulatedHeadset_getEyePose)(RSSimulatedHeadset* self, SEL sel,
                                                  struct RSSimulatedHeadsetPose* pose, int forEye);
static void hook_RSSimulatedHeadset_getEyePose(RSSimulatedHeadset* self, SEL sel,
                                               struct RSSimulatedHeadsetPose* pose, int forEye) {
  real_RSSimulatedHeadset_getEyePose(self, sel, pose, forEye);
  
  /*if (forEye == 0)
  {
    pose->position = left_eye_pos;
    pose->rotation = left_eye_quat;
  }
  else if (forEye == 1)
  {
    pose->position = right_eye_pos;
    pose->rotation = right_eye_quat;
  }*/
  /*else if (forEye == 2)
  {
    pose->position = left_eye_pos;
    pose->rotation = left_eye_quat;
  }*/
  
  //printf("get eye pos %u, %f %f %f %f\n", forEye, pose->position[0], pose->position[1], pose->position[2], pose->position[3]);
}

static void (*real_RSSimulatedHeadset_setEyePose)(RSSimulatedHeadset* self, SEL sel,
                                                  struct RSSimulatedHeadsetPose pose, int forEye);
static void hook_RSSimulatedHeadset_setEyePose(RSSimulatedHeadset* self, SEL sel,
                                               struct RSSimulatedHeadsetPose pose, int forEye) {
  /*if (forEye == 0)
  {
    pose.position = left_eye_pos;
    pose.rotation = left_eye_quat;
  }
  else if (forEye == 1)
  {
    pose.position = right_eye_pos;
    pose.rotation = right_eye_quat;
  }*/
  /*else if (forEye == 2)
  {
    pose.position = left_eye_pos;
    pose.rotation = left_eye_quat;
  }*/

  real_RSSimulatedHeadset_setEyePose(self, sel, pose, forEye);
  
  //printf("set eye pos %u, %f %f %f %f\n", forEye, pose.position[0], pose.position[1], pose.position[2], pose.position[3]);
}


static void hook_RSSimulatedHeadset_setHMDPose(RSSimulatedHeadset* self, SEL sel,
                                               struct RSSimulatedHeadsetPose pose) {
  if (getenv("XRHAX_CAPTURE_PAUSED")) {
    real_RSSimulatedHeadset_setHMDPose(self, sel, pose);
    return;
  }
  static dispatch_once_t logged;
  dispatch_once(&logged, ^{ NSLog(@"[Sim2OpenXR] Headset pose callback active."); });
  //pose.position = left_eye_pos[pulled_which];
  //pose.rotation = left_eye_quat[pulled_which];

  gHookedSimulatedHeadset = self;
  gHookedSimulatedHeadset_sel = sel;

  pull_openxr_data();
  pthread_mutex_lock(&capture_pose_mutex);
  if (pulled_which < 0) {
    pthread_mutex_unlock(&capture_pose_mutex);
    real_RSSimulatedHeadset_setHMDPose(self, sel, pose);
    return;
  }
  pose.position = left_eye_pos[pulled_which];
  pose.rotation = left_eye_quat[pulled_which];
  pthread_mutex_unlock(&capture_pose_mutex);

  real_RSSimulatedHeadset_setHMDPose(self, sel, pose);
  
  //printf("set hmd pos %f %f %f %f\n", pose.position[0], pose.position[1], pose.position[2], pose.position[3]);
}

static void (*real_RSSimulatedHeadset_getPose)(RSSimulatedHeadset* self, SEL sel,
                                                  struct RSSimulatedHeadsetPose* pose, double time);
static void hook_RSSimulatedHeadset_getPose(RSSimulatedHeadset* self, SEL sel,
                                               struct RSSimulatedHeadsetPose* pose, double time) {
  real_RSSimulatedHeadset_getPose(self, sel, pose, time);
  if (pulled_which < 0) return;

  //pull_openxr_data();
  //printf("RSSimulatedHeadset_getPose\n");

  pose->position = left_eye_pos[pulled_which];
  pose->rotation = left_eye_quat[pulled_which];

  real_RSSimulatedHeadset_setHMDPose(self, sel, *pose);
}

static BOOL require_method(Class cls, SEL selector) {
  if (cls && class_getInstanceMethod(cls, selector)) return YES;
  NSLog(@"[Sim2OpenXR] Unsupported runtime: missing %@ %@. Pose hooks disabled.",
        cls ? NSStringFromClass(cls) : @"RSSimulatedHeadset", NSStringFromSelector(selector));
  return NO;
}

__attribute__((constructor)) static void SetupSignalHandler() {
  NSLog(@"[Sim2OpenXR] Bridge loaded (experimental modern simulator port).");
  Class headsetClass = NSClassFromString(@"RSSimulatedHeadset");
  // getPose:atTime: was removed in visionOS 27. setHMDPose: still supplies
  // the required tracking path; the getter/setter pass-through hooks are optional.
  if (!require_method(headsetClass, @selector(setHMDPose:))) return;
  static dispatch_queue_t signal_queue;
  static dispatch_source_t signal_source;
  signal_queue = dispatch_queue_create("com.worthdoingbadly.stereoscreenshots.signalqueue",
                                       DISPATCH_QUEUE_SERIAL);
  signal_source =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, /*mask=*/0, signal_queue);
  dispatch_source_set_event_handler(signal_source, ^{
    // TODO reuse this?
  });
  signal(SIGUSR1, SIG_IGN);
  dispatch_activate(signal_source);

#if 0
  {
    Class cls = NSClassFromString(@"RSUserSettings");
    Method method = class_getInstanceMethod(cls, @selector(worstAllowedFrameTime));
    real_RSUserSettings_worstAllowedFrameTime = (void*)method_getImplementation(method);
    method_setImplementation(method, (IMP)hook_RSUserSettings_worstAllowedFrameTime);
  }
#endif

  {
    Class cls = NSClassFromString(@"RSSimulatedHeadset");
    Method method = class_getInstanceMethod(cls, @selector(_getEyePose:forEye:));
    if (method) {
      real_RSSimulatedHeadset_getEyePose = (void*)method_getImplementation(method);
      method_setImplementation(method, (IMP)hook_RSSimulatedHeadset_getEyePose);
    }
  }

#if 1
  {
    Class cls = NSClassFromString(@"RSSimulatedHeadset");
    Method method = class_getInstanceMethod(cls, @selector(getPose:atTime:));
    if (method) {
      real_RSSimulatedHeadset_getPose = (void*)method_getImplementation(method);
      method_setImplementation(method, (IMP)hook_RSSimulatedHeadset_getPose);
    }
  }
#endif

  {
    Class cls = NSClassFromString(@"RSSimulatedHeadset");
    Method method = class_getInstanceMethod(cls, @selector(setEyePose:forEye:));
    if (method) {
      real_RSSimulatedHeadset_setEyePose = (void*)method_getImplementation(method);
      method_setImplementation(method, (IMP)hook_RSSimulatedHeadset_setEyePose);
    }
  }

  {
    Class cls = NSClassFromString(@"RSSimulatedHeadset");
    Method method = class_getInstanceMethod(cls, @selector(setHMDPose:));
    real_RSSimulatedHeadset_setHMDPose = (void*)method_getImplementation(method);
    method_setImplementation(method, (IMP)hook_RSSimulatedHeadset_setHMDPose);
  }

#if 1
  {
    Class cls = NSClassFromString(@"RSXRRenderLoop");
    Method method = class_getInstanceMethod(cls, @selector(currentFrequency));
    if (method) {
      real_RSXRRenderLoop_currentFrequency = (void*)method_getImplementation(method);
      method_setImplementation(method, (IMP)hook_RSXRRenderLoop_currentFrequency);
    }
  }
#endif

  //openxr_main();
  NSLog(@"[Sim2OpenXR] Pose hooks installed.");
}

int redirect_nslog(const char *prefix, const char *buffer, int size)
{
    NSLog(@"%s (%d bytes): %.*s", prefix, size, size, buffer);
    return size;
}

int stderr_redirect_nslog(void *inFD, const char *buffer, int size)
{
    return redirect_nslog("stderr", buffer, size);
}

int stdout_redirect_nslog(void *inFD, const char *buffer, int size)
{
    return redirect_nslog("stdout", buffer, size);
}
