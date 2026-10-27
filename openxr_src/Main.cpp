#include "../openxr-Bridging-Header.h"
#include "../simui/simui_src/simui_types.h"
#include "../simui/simui_src/sharedmem_buffer.h"
#include "Context.h"
#include "Headset.h"
#include "Renderer.h"
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <glm/gtc/type_ptr.hpp>
#include <chrono>
#include <thread>
#include <deque>
#include <semaphore>
#include "Util.h"

int xr_begin_frame_valid = 0;
int last_rendered_idx = 0;
int pose_data_valid = 0;
std::mutex renderMutex[3];
std::mutex renderMutex2;
std::mutex poseLatchMutex;
std::mutex endFrameMutex;
std::mutex contextInitMutex;
std::binary_semaphore beginFrameSem(0);
std::binary_semaphore frameCompleteSem(0);
std::mutex dataMutex[3];
std::binary_semaphore xrosRenderDone[3] = {std::binary_semaphore(0),std::binary_semaphore(0),std::binary_semaphore(0)}; 
std::binary_semaphore xrosRenderDoneAck[3] = {std::binary_semaphore(0),std::binary_semaphore(0),std::binary_semaphore(0)}; 

extern "C"
{
  #include <libusb.h>

  int redirect_nslog(const char *prefix, const char *buffer, int size);
  int stderr_redirect_nslog(void *inFD, const char *buffer, int size);
  int stdout_redirect_nslog(void *inFD, const char *buffer, int size);

  //extern void dyld_all_image_infos();
  void *getDyldBase(void) {
    struct task_dyld_info dyld_info;
    mach_vm_address_t image_infos;
    struct dyld_all_image_infos *infos;
    
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t ret;
    
    ret = task_info(mach_task_self_,
                    TASK_DYLD_INFO,
                    (task_info_t)&dyld_info,
                    &count);
    
    if (ret != KERN_SUCCESS) {
        return NULL;
    }
    
    image_infos = dyld_info.all_image_info_addr;
    
    infos = (struct dyld_all_image_infos *)image_infos;
    return (void*)infos->dyldImageLoadAddress;
}

static void print_devs(libusb_device **devs)
{
  libusb_device *dev;
  int i = 0, j = 0;
  uint8_t path[8]; 

  while ((dev = devs[i++]) != NULL) {
    struct libusb_device_descriptor desc;
    int r = libusb_get_device_descriptor(dev, &desc);
    if (r < 0) {
      fprintf(stderr, "failed to get device descriptor");
      return;
    }

    printf("%04x:%04x (bus %d, device %d)",
      desc.idVendor, desc.idProduct,
      libusb_get_bus_number(dev), libusb_get_device_address(dev));

    r = libusb_get_port_numbers(dev, path, sizeof(path));
    if (r > 0) {
      printf(" path: %d", path[0]);
      for (j = 1; j < r; j++)
        printf(".%d", path[j]);
    }
    printf("\n");
  }
}

int libusb_test_main(void)
{
  libusb_device **devs;
  int r;
  ssize_t cnt;

  r = libusb_init(/*ctx=*/NULL);
  if (r < 0)
    return r;

  cnt = libusb_get_device_list(NULL, &devs);
  printf("Found %u devices\n", cnt);
  if (cnt < 0){
    libusb_exit(NULL);
    return (int) cnt;
  }

  print_devs(devs);
  libusb_free_device_list(devs, 1);

  libusb_exit(NULL);
  return 0;
}
}

MTLTexture_id dummy_tex[3];

MTLTexture_id* g_paTex_l = nullptr;
MTLTexture_id* g_paTex_r = nullptr;
uint32_t g_tex_w = 0;
uint32_t g_tex_h = 0;
Context* context = nullptr;
Headset* headset = nullptr;
Renderer* renderer = nullptr;
int openxr_is_done = 0;
std::thread* pose_thread;
void* shm_addr;

int current_pose_idx = 0;

 std::chrono::time_point<std::chrono::high_resolution_clock> last_loop;

#define DO_MIRROR

extern "C" int openxr_init();

extern "C" int openxr_main()
{
  printf("XRHax: OpenXR main!\n");
  //openxr_init();
  //while (!openxr_loop()) {}
  printf("XRHax: OpenXR main done!\n");
  return 0;
}

extern "C" int pose_fetch_loop()
{
  Headset::BeginFrameResult frameResult;
  int new_idx = 0;
  while(1)
  {
    //printf("pose fetch endframe wait\n");
    endFrameMutex.lock();
    //printf("pose fetch endframe get\n");
    for (int i = 0; i < 3; i++)
    {
      frameResult = headset->beginFrame(&new_idx);
      //printf("pose fetch got frame\n");
      if (frameResult == Headset::BeginFrameResult::RenderFully)
      {
        //printf("begin frame valid\n");
        xr_begin_frame_valid = 1;
        beginFrameSem.release();
        break;
      }
      else if (frameResult == Headset::BeginFrameResult::SkipRender)
      {
        headset->endFrame(new_idx);
        continue;
      }
    }
    
    endFrameMutex.unlock();
    
    //printf("pose fetch latch wait\n");
    poseLatchMutex.lock();
    //printf("pose fetch latch get\n");
    if (frameResult == Headset::BeginFrameResult::RenderFully)
    {
      current_pose_idx = new_idx;
      pose_data_valid = 1;
    }
    poseLatchMutex.unlock();

    // Keep the OpenXR frame and its pose storage stable until the captured
    // image has been submitted. Repeated xrBeginFrame discards older frames.
    if (frameResult == Headset::BeginFrameResult::RenderFully) {
      frameCompleteSem.acquire();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static std::mutex monoProjectionMutex;
static bool monoProjectionValid = false;
static float monoProjectionAngles[4];

extern "C" void openxr_set_mono_tangents(float left, float right, float up, float down)
{
  if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(up) || !std::isfinite(down) ||
      left <= 0 || right <= 0 || up <= 0 || down <= 0) return;
  std::lock_guard<std::mutex> lock(monoProjectionMutex);
  monoProjectionAngles[0] = -std::atan(left);
  monoProjectionAngles[1] = std::atan(right);
  monoProjectionAngles[2] = std::atan(up);
  monoProjectionAngles[3] = -std::atan(down);
  monoProjectionValid = true;
}

extern "C" int openxr_get_mono_fov(float *angles)
{
  std::lock_guard<std::mutex> lock(monoProjectionMutex);
  if (!monoProjectionValid || !angles) return 0;
  memcpy(angles, monoProjectionAngles, sizeof(monoProjectionAngles));
  return 1;
}

extern "C" int openxr_init()
{
  setlinebuf(stdout);
  setlinebuf(stderr);
  stdout->_write = stdout_redirect_nslog;
  stderr->_write = stderr_redirect_nslog;
  clearerr(stdout);
  clearerr(stderr);
  //setenv("XR_RUNTIME_JSON", "/Users/maxamillion/workspace/XRGyroControls_OpenXR_2/openxr_monado-dev.json", 1);

  //frame_started = 0;
  /*for (int i = 0; i < 3; i++)
  {
    renderMutex2[i].lock();
    //xrosRenderDone[i].acquire();
  }*/
  

  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s/IOUSBLib_ios_hax.dylib", getenv("XRHAX_ROOTDIR") ? getenv("XRHAX_ROOTDIR") : ".");
  const char* pszModule = tmp;
  void* h = dlopen(pszModule, RTLD_GLOBAL);
  if (h == NULL)
  {
      printf("Failed to dlopen %s.  %s\n", pszModule, dlerror() );
  }

  {
    // Open the shared memory, with required access mode and permissions.
    // This is analagous to how we open file with open()
    int fd = xr_open_shared_memory();
    if (fd < 0) {
      char error[256];
      int length = snprintf(error, sizeof(error), "Shared memory initialization failed: %s", strerror(errno));
      redirect_nslog("[Sim2OpenXR]", error, length);
      return EXIT_FAILURE;
    }

    // map the shared memory to an address in the virtual address space
    // with the file descriptor of the shared memory and necessary protection.
    // The flags to mmap must be set to MAP_SHARED for other process to access.
    shm_addr = mmap(NULL, 1048576, PROT_READ | PROT_WRITE /*protection*/, MAP_SHARED /*flags*/, fd, 0);
    close(fd);
    if (shm_addr == MAP_FAILED) {
      shm_addr = nullptr;
      perror("[Sim2OpenXR] mmap");
      return EXIT_FAILURE;
    }
    // Do not feed a previous (possibly simulated) session's pose to the input helper.
    memset(shm_addr, 0, sizeof(sharedmem_data));
  
    printf("SimUI sharedmem startup: %u %p %x\n", fd, shm_addr, *(uint32_t*)shm_addr);
  }

  printf("XRHax: OpenXR init!\n");
  context = new Context();
  if (!context || !context->isValid())
  {
    return EXIT_FAILURE;
  }

  printf("XRHax: createDevice!\n");

  if (!context->createDevice())
  {
    return EXIT_FAILURE;
  }

  printf("XRHax: create Headset!\n");

  headset = new Headset(context);
  if (!headset || !headset->isValid())
  {
    return EXIT_FAILURE;
  }

  printf("XRHax: create Renderer!\n");

  if (!g_paTex_l) {
    g_paTex_l = dummy_tex;
    g_tex_w = 512;
  }
  if (!g_paTex_r) {
    g_paTex_r = dummy_tex;
    g_tex_h = 512;
  }

  renderer = new Renderer(context, headset, g_paTex_l, g_paTex_r, g_tex_w, g_tex_h);
  if (!renderer || !renderer->isValid())
  {
    printf("XRHax: create Renderer failed!\n");
    return EXIT_FAILURE;
  }

  printf("XRHax: OpenXR init success!\n");

  pose_thread = new std::thread(pose_fetch_loop);

  return EXIT_SUCCESS;
}

extern "C" int openxr_set_textures(MTLTexture_id* paTex_l, MTLTexture_id* paTex_r, uint32_t w, uint32_t h)
{
  if (openxr_is_done) return 1;
  g_paTex_l = paTex_l;
  g_paTex_r = paTex_r;
  g_tex_w = w;
  g_tex_h = h;

  contextInitMutex.lock();
  if (!context || !context->isValid()) {
    if(openxr_init() != EXIT_SUCCESS) {
      contextInitMutex.unlock();
      openxr_is_done = 1;
      return 1;
    }
  }
  contextInitMutex.unlock();

  if (!renderer) return 1;

  renderer->metal_tex_l[0] = g_paTex_l[0];
  renderer->metal_tex_l[1] = g_paTex_l[1];
  renderer->metal_tex_l[2] = g_paTex_l[2];
  renderer->metal_tex_r[0] = g_paTex_r[0];
  renderer->metal_tex_r[1] = g_paTex_r[1];
  renderer->metal_tex_r[2] = g_paTex_r[2];
  renderer->metal_tex_w = g_tex_w;
  renderer->metal_tex_h = g_tex_h;
  return 0;
}

uint32_t swapchainImageIndex[3] = {0,0,0};
int64_t last_display_time = 0;

extern "C" int openxr_full_loop(int which, int poseIdx)
{
  struct CompletionSignal {
    ~CompletionSignal() { frameCompleteSem.release(); }
  } completionSignal;
  contextInitMutex.lock();
  if (!context || !context->isValid()) {
    if(openxr_init() != EXIT_SUCCESS) {
      contextInitMutex.unlock();
      openxr_is_done = 1;
      return 1;
    }
  }
  contextInitMutex.unlock();

  // Main loop
  if (headset->isExitRequested()) {
    openxr_is_done = 1;
    return 1;
  }

  xrosRenderDone[which].acquire();
  while (xrosRenderDone[which].try_acquire());
  //xrosRenderDoneAck[which].release();

  //printf("%u wait renderMutex2 (idx %u)\n", which, poseIdx);
  renderMutex2.lock();

  

  if ((headset->storedPoses[poseIdx].frameState.predictedDisplayTime < last_display_time)
      /*|| (headset->storedPoses[poseIdx].frameState.predictedDisplayTime == last_display_time && poseIdx != (last_rendered_idx+1) % STORED_POSE_COUNT)*/
      ) {
    printf("%u out of order (idx %u) %llx %llx\n", which, poseIdx, headset->storedPoses[poseIdx].frameState.predictedDisplayTime, last_display_time);
    //endFrameMutex.unlock();
    //renderMutex2.unlock();
    //return 0;
  }

  //renderMutex.lock();

  //std::lock_guard<std::mutex> guard(renderMutex);

  if (which == 0)
  {
    static int throttle_prints = 0;
    auto elapsed = std::chrono::high_resolution_clock::now() - last_loop;
    long long microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (++throttle_prints > 30) 
    {
      printf("%zu microseconds loop\n", (size_t)microseconds / 3);
      throttle_prints = 0;
    }
    last_loop = std::chrono::high_resolution_clock::now();
  }

  //printf("%u wait render (idx %u)\n", which, poseIdx);
  
  int acquired = 0;
  for (int i = 0; i < 100; i++) {
    if(beginFrameSem.try_acquire()) {
      //printf("%u acquired at %u (idx %u)\n", which, i, poseIdx);
      acquired = 1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!acquired) {
    printf("%u failed to acquire (idx %u)\n", which, poseIdx);
    renderMutex2.unlock();

    return 0;
  }
  //poseLatchMutex.lock();
  
#if 0
  if (!xr_begin_frame_valid) {
    /*poseLatchMutex.unlock();
    //headset->redoBeginFrame();
    while (!xr_begin_frame_valid) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    poseLatchMutex.lock();*/
    poseLatchMutex.unlock();
    renderMutex2.unlock();
    return 0;
  }
#endif

  /*if (last_rendered_idx == poseIdx) {
    poseLatchMutex.unlock();
    renderMutex2.unlock();
    return 0;
  }*/
  

  //printf("%u wait endFrameMutex (idx %u)\n", which, poseIdx);
  
  endFrameMutex.lock();

  // Apparently the max value in std::counting_semaphore is just there to look pretty??
  // Drain the semaphore.
  while (beginFrameSem.try_acquire());

  // possible race condition
  if (!xr_begin_frame_valid) {
    printf("%u !xr_begin_frame_valid (idx %u)\n", which, poseIdx);
    endFrameMutex.unlock();
    renderMutex2.unlock();
    return 0;
    //int new_idx = 0;
    //headset->beginFrame(&new_idx);
  }
  //printf("%u render (idx %u)\n", which, poseIdx);
  headset->beginFrameRender(swapchainImageIndex[which]);
  //printf("%u render beginframe done (idx %u)\n", which, poseIdx);
  renderer->render(swapchainImageIndex[which], which);
  
  //printf("%u render render done (idx %u)\n", which, poseIdx);
  //printf("submit %u\n", which);
  renderer->submit(false, which);
  //printf("%u render submit done (idx %u)\n", which, poseIdx);
  headset->endRender(poseIdx);
  //printf("%u render endRender done (idx %u)\n", which, poseIdx);
  headset->endFrame(poseIdx);
  static bool logged_first_frame = false; // Protected by renderMutex2.
  if (!logged_first_frame) {
    printf("[Sim2OpenXR] First OpenXR video frame submitted.\n");
    logged_first_frame = true;
  }
  //printf("%u render endframe done (idx %u)\n", which, poseIdx);

  last_display_time = headset->storedPoses[poseIdx].frameState.predictedDisplayTime;
  last_rendered_idx = poseIdx;

  xr_begin_frame_valid = 0;
  endFrameMutex.unlock();
  //printf("%u render done (idx %u)\n", which, poseIdx);

  //std::this_thread::sleep_for(std::chrono::milliseconds(508));
  renderMutex2.unlock();

  return 0;
}

int fake_loop(int which, int poseIdx)
{
  setlinebuf(stdout);
  setlinebuf(stderr);
  stdout->_write = stdout_redirect_nslog;
  stderr->_write = stderr_redirect_nslog;
  //printf("Fake loop\n");

  //xrosRenderDone[which].acquire();
  //while (xrosRenderDone[which].try_acquire());
  //printf("Fake loop 2\n");

  renderMutex2.lock();
  //printf("Fake loop 3\n");
  
  endFrameMutex.lock();
  //printf("Fake loop 4\n");
  //while (beginFrameSem.try_acquire());

  endFrameMutex.unlock();
  renderMutex2.unlock();
  //printf("Fake loop exit\n");
  return 0;
}

extern "C" void openxr_spawn_renderframe(int which, int poseIdx)
{
  contextInitMutex.lock();
  if (!context || !context->isValid()) {
    contextInitMutex.unlock();
    fake_loop(which, poseIdx);
    return;
  }
  contextInitMutex.unlock();
  
  //std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  std::thread(openxr_full_loop, which, poseIdx).detach();
  //openxr_full_loop(which, poseIdx);
#if 0
  for (int i = 0; i < 40000; i++)
  {
    if (renderMutex[0].try_lock()) {
      //printf("spawn render...\n");
      std::thread(openxr_full_loop, which, poseIdx).detach();
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  printf("couldn't spawn render %u...\n", which);
  //dataMutex[which].unlock();
#endif
}

extern "C" void openxr_complete_renderframe(int which, int poseIdx)
{
  contextInitMutex.lock();
  if (!context || !context->isValid()) {
    //return;
  }
  contextInitMutex.unlock();
  //printf("frame completed (%u %u), unlocking\n", which, poseIdx);
  //renderMutex2[which].unlock();
  xrosRenderDone[which].release();
  //xrosRenderDoneAck[which].acquire();
}

extern "C" int openxr_done()
{
  return openxr_is_done;
}

extern "C" int openxr_cleanup()
{
  if (!context) return 0;
  context->sync();

  delete renderer;
  delete headset;
  delete context;

  context = nullptr;
  headset = nullptr;
  renderer = nullptr;
  return 0;
}

float offset_x, offset_y, offset_z;
bool offsets_set = false;

extern "C" int openxr_headset_get_data(openxr_headset_data* out, int which)
{
  if (!out) return 0;
  out->view_l = NULL;
  out->view_r = NULL;
  out->proj_l = NULL;
  out->proj_r = NULL;
  out->tangents_l = NULL;
  out->tangents_r = NULL;
  if (!headset) return -1;

  //dataMutex[which].lock();

  //printf("%u wait data\n", which);
  poseLatchMutex.lock();
  if (!pose_data_valid) {
    poseLatchMutex.unlock();
    return -1;
  }
  int poseIdx = current_pose_idx;
  PoseData* pPose = &headset->storedPoses[current_pose_idx];
  pose_data_valid = 0;
  poseLatchMutex.unlock();
  
  //printf("%u get data (idx %u)\n", which, poseIdx);
  //printf("pulling headset data for %u, idx %u\n", which, poseIdx);

  glm::mat4 ctrl_l = util::poseToMatrix(pPose->tracked_locations[0].pose);
  glm::mat4 ctrl_r = util::poseToMatrix(pPose->tracked_locations[1].pose);

  //std::lock_guard<std::mutex> guard(pPose->eyePoseMutex);

  const XrView& eyePose0 = pPose->eyePoses.at(0);
  const XrView& eyePose1 = pPose->eyePoses.at(1);

  if (!offsets_set) {
    offset_x = eyePose0.pose.position.x;
    //offset_y = /*eyePose0.pose.position.y +*/ 0.2;
    offset_y = 1.5;
    offset_z = eyePose0.pose.position.z - 0.5;
    offsets_set = true;
  }

  if (pPose->grab_value[0].currentState > 0.5) {
      //offset_y = 2.22;
      //printf("Down!\n");
  }
  else {
    //printf("Not down!\n");
    offset_y = 1.5;
  }

  out->l_x  = eyePose0.pose.position.x - offset_x;
  out->l_y  = eyePose0.pose.position.y - offset_y;
  out->l_z  = eyePose0.pose.position.z - offset_z;
  out->l_qx = eyePose0.pose.orientation.x;
  out->l_qy = eyePose0.pose.orientation.y;
  out->l_qz = eyePose0.pose.orientation.z;
  out->l_qw = eyePose0.pose.orientation.w;

  out->r_x  = eyePose1.pose.position.x - offset_x;
  out->r_y  = eyePose1.pose.position.y - offset_y;
  out->r_z  = eyePose1.pose.position.z - offset_z;
  out->r_qx = eyePose1.pose.orientation.x;
  out->r_qy = eyePose1.pose.orientation.y;
  out->r_qz = eyePose1.pose.orientation.z;
  out->r_qw = eyePose1.pose.orientation.w;

  out->view_l = glm::value_ptr(pPose->eyeViewMatrices.at(0));
  out->view_r = glm::value_ptr(pPose->eyeViewMatrices.at(1));
  out->proj_l = glm::value_ptr(pPose->eyeProjectionMatrices.at(0));
  out->proj_r = glm::value_ptr(pPose->eyeProjectionMatrices.at(1));
  out->tangents_l = pPose->eyeTangents_l;
  out->tangents_r = pPose->eyeTangents_r;

  out->view_l[12] = out->l_x;
  out->view_l[13] = out->l_y;
  out->view_l[14] = out->l_z;
  out->view_r[12] = out->r_x;
  out->view_r[13] = out->r_y;
  out->view_r[14] = out->r_z;

  glm::mat4 view_r_rel = glm::inverse(pPose->eyeViewMatrices.at(0)) * pPose->eyeViewMatrices.at(1);
  memcpy(out->view_r_rel, glm::value_ptr(view_r_rel), sizeof(out->view_r_rel));

  memcpy(out->l_controller, glm::value_ptr(ctrl_l), sizeof(out->l_controller));
  memcpy(out->r_controller, glm::value_ptr(ctrl_r), sizeof(out->r_controller));

  out->l_controller[12] -= offset_x;
  out->l_controller[13] -= offset_y;
  out->l_controller[14] -= offset_z;
  out->r_controller[12] -= offset_x;
  out->r_controller[13] -= offset_y;
  out->r_controller[14] -= offset_z;

  if (shm_addr) {
    sharedmem_data* dat = (sharedmem_data*)shm_addr;
    dat->l_x  = out->l_x;
    dat->l_y  = out->l_y;
    dat->l_z  = out->l_z;
    dat->l_qx = out->l_qx;
    dat->l_qy = out->l_qy;
    dat->l_qz = out->l_qz;
    dat->l_qw = out->l_qw;

    dat->r_x  = out->r_x;
    dat->r_y  = out->r_y;
    dat->r_z  = out->r_z;
    dat->r_qx = out->r_qx;
    dat->r_qy = out->r_qy;
    dat->r_qz = out->r_qz;
    dat->r_qw = out->r_qw;

    glm::quat l_q(dat->l_qx, dat->l_qy, dat->l_qz, dat->l_qw);
    glm::quat r_q(dat->r_qx, dat->r_qy, dat->r_qz, dat->r_qw);

    memcpy(dat->l_view, out->view_l, sizeof(dat->l_view));
    memcpy(dat->r_view, out->view_r, sizeof(dat->r_view));

    //dat->grab_val[0] = pPose->pinch_l ? 0.9 : 0.0;//pPose->grab_value[0].currentState;
    //dat->grab_val[1] = pPose->pinch_r ? 0.9 : 0.0;//pPose->grab_value[1].currentState;
    dat->grab_val[0] = pPose->grab_value[0].currentState;
    dat->grab_val[1] = pPose->grab_value[1].currentState;
    dat->grip_val[0] = pPose->grip_value[0].currentState;
    dat->grip_val[1] = pPose->grip_value[1].currentState;

    memcpy(dat->l_controller, out->l_controller, sizeof(dat->l_controller));
    memcpy(dat->r_controller, out->r_controller, sizeof(dat->r_controller));
    //printf("grabs %f %f\n", dat->grab_val[0], dat->grab_val[1]);

    memcpy(dat->gaze_mat, glm::value_ptr(pPose->l_eye_mat), sizeof(dat->gaze_mat));

    glm::quat gaze_quat = pPose->l_eye_quat;
    gaze_quat = l_q * gaze_quat;
    dat->gaze_quat[0] = gaze_quat.x;
    dat->gaze_quat[1] = gaze_quat.y;
    dat->gaze_quat[2] = gaze_quat.z;
    dat->gaze_quat[3] = gaze_quat.w;
    dat->l_controller_quat[0] = pPose->tracked_locations[0].pose.orientation.x;
    dat->l_controller_quat[1] = pPose->tracked_locations[0].pose.orientation.y;
    dat->l_controller_quat[2] = pPose->tracked_locations[0].pose.orientation.z;
    dat->l_controller_quat[3] = pPose->tracked_locations[0].pose.orientation.w;
    dat->r_controller_quat[0] = pPose->tracked_locations[1].pose.orientation.x;
    dat->r_controller_quat[1] = pPose->tracked_locations[1].pose.orientation.y;
    dat->r_controller_quat[2] = pPose->tracked_locations[1].pose.orientation.z;
    dat->r_controller_quat[3] = pPose->tracked_locations[1].pose.orientation.w;

    glm::vec3 z_vec = glm::vec3(-dat->gaze_mat[8], -dat->gaze_mat[9], -dat->gaze_mat[10]);

    dat->system_button = pPose->system_button ? 1 : 0;
    dat->menu_button = pPose->menu_button ? 1 : 0;
    dat->left_touch_button = pPose->left_touch_button ? 1 : 0;
    dat->right_touch_button = pPose->right_touch_button ? 1 : 0;

    memcpy(dat->gaze_vec, glm::value_ptr(z_vec), sizeof(dat->gaze_vec));

    //printf("Left:  %f %f %f %f\n", z_vec_filtered[0], z_vec_filtered[1], z_vec_filtered[2]);
  }
  return poseIdx;
}
