#include "utils.h"
#include "luaT.h"
#include "THCGeneral.h"
#include "THCTensorRandom.h"

extern void cutorch_CudaStorage_init(lua_State* L);
extern void cutorch_CudaTensor_init(lua_State* L);
extern void cutorch_CudaTensorMath_init(lua_State* L);
extern void cutorch_CudaTensorOperator_init(lua_State* L);

/*
   Iteration utilities for lists of streams and lists of gpus with streams
*/

int checkAndCountListOfStreams(lua_State *L, THCState *state, int arg,
                               int device)
{
  if (!lua_istable(L, arg)) {
    THError("expecting table of device streams");
  }

  /* Push table to top */
  lua_pushvalue(L, arg);

  /* Check that all values in the table are numeric and in bounds */
  int streams = 0;
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (!lua_isnumber(L, -1)) {
      THError("streamWaitFor: list of streams must be numeric");
    }
    int streamId = (int) lua_tonumber(L, -1);

    /* This will error out if the stream is not in bounds */
    THCState_getDeviceStream(state, device, streamId);

    ++streams;
    lua_pop(L, 1);
  }

  /* Pop table from top */
  lua_pop(L, 1);
  return streams;
}

void checkAndCountListOfGPUStreamPairs(lua_State *L, THCState *state, int arg,
                                       int* gpus,
                                       int* streams)
{
  if (!lua_istable(L, arg)) {
    THError("expecting table of gpu={streams...}");
  }

  /* Push table to top */
  lua_pushvalue(L, arg);

  /* Check that all values in the table are tables of numeric and in bounds */
  *gpus = 0;
  *streams = 0;

  lua_pushnil(L);
  while (lua_next(L, -2)) {
    /* -2 is key (device), -1 is value, in the form device={streams...} */
    if (!lua_isnumber(L, -2) || !lua_istable(L, -1)) {
      THError("expecting table of gpu={streams...}");
    }

    int device = (int) lua_tonumber(L, -2) - 1;
    /* Verify device is in range */
    if (device < 0 || device >= THCState_getNumDevices(state)) {
      THError("%d is not a device", device + 1);
    }

    /* Verify that the list is a list of streams */
    *streams += checkAndCountListOfStreams(L, state, -1, device);
    ++(*gpus);
    lua_pop(L, 1);
  }

  /* Pop table from top */
  lua_pop(L, 1);
}

void createSingleDeviceEvent(lua_State *L, THCState *state, int arg,
                             int device, cudaEvent_t* event)
{
  THCudaCheck(cudaEventCreateWithFlags(event, cudaEventDisableTiming));

  /* Push table to top */
  lua_pushvalue(L, arg);

  /* Record events */
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    int streamId = (int) lua_tonumber(L, -1);
    cudaStream_t streamWaitingOn =
      THCState_getDeviceStream(state, device, streamId);
    THCudaCheck(cudaEventRecord(*event, streamWaitingOn));
    lua_pop(L, 1);
  }

  /* Pop table from top */
  lua_pop(L, 1);
}

void createMultiDeviceEvents(lua_State *L, THCState *state, int arg,
                             cudaEvent_t* events)
{
  /* Push {gpu={streams...}} table */
  lua_pushvalue(L, arg);

  /* Create and record events per each GPU */
  int gpu = 0;
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    int device = (int) lua_tonumber(L, -2) - 1;
    THCudaCheck(cudaSetDevice(device));
    createSingleDeviceEvent(L, state, -1, device, &events[gpu]);
    ++gpu;

    lua_pop(L, 1);
  }

  /* Pop {gpu={streams...}} table */
  lua_pop(L, 1);
}

void waitSingleDeviceEvent(lua_State *L, THCState *state, int arg,
                           int device, cudaEvent_t event)
{
  /* Push table to top */
  lua_pushvalue(L, arg);

  /* Then, wait on the events. Each stream is actually waiting on itself here
     too, but that's harmless and isn't worth weeding out. */
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    int streamId = (int) lua_tonumber(L, -1);
    cudaStream_t stream =
      THCState_getDeviceStream(state, device, streamId);
    THCudaCheck(cudaStreamWaitEvent(stream, event, 0));
    lua_pop(L, 1);
  }

  /* Pop table from top */
  lua_pop(L, 1);
}

void waitMultiDeviceEvents(lua_State *L, THCState *state, int arg,
                           cudaEvent_t* events, int gpus)
{
  /* Push {gpu={streams...}} table */
  lua_pushvalue(L, arg);

  /* Then, wait on the events. Each stream is actually waiting on itself here
     too, but that's harmless and isn't worth weeding out. */
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    int device = (int) lua_tonumber(L, -2) - 1;
    THCudaCheck(cudaSetDevice(device));

    /* Push stream table */
    lua_pushvalue(L, -1);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      int streamId = (int) lua_tonumber(L, -1);

      cudaStream_t stream =
        THCState_getDeviceStream(state, device, streamId);

      /* Each stream waits on all events */
      for (int i = 0; i < gpus; ++i) {
        THCudaCheck(cudaStreamWaitEvent(stream, events[i], 0));
      }

      lua_pop(L, 1);
    }

    /* Pop stream table and GPU entry */
    lua_pop(L, 2);
  }

  /* Pop {gpu={streams...}} table */
  lua_pop(L, 1);
}

static int cutorch_synchronize(lua_State *L)
{
  THCudaCheck(cudaDeviceSynchronize());
  return 0;
}

/*
   Usage:
   cutorch.reserveStreams(n)
   Allocates n user streams for every device present. If fewer than
   n streams are currently allocated, an additional number will be added.
   If more than n streams are currently allocated, does nothing.
   The default CUDA stream is assumed to be stream 0 and is always present;
   the allocated streams are user streams on top of the CUDA streams
   (thus, reserveStreams(1) will create 1 user stream with two being available,
   the default stream 0 and the user stream 1, on each device).
*/
static int cutorch_reserveStreams(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  int numStreams = (int) luaL_checknumber(L, 1);
  THCState_reserveStreams(state, numStreams);

  return 0;
}

/*
   Usage:
   n = cutorch.getNumStreams()
   Returns the number of user streams allocated for every device present.
   By default, is 0.
*/
static int cutorch_getNumStreams(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  lua_pushnumber(L, THCState_getNumStreams(state));

  return 1;
}

/*
   Usage:
   cutorch.setStream(n)
   For all devices, sets the current user stream in use to the index
   specified. e.g.,
   ---
   cutorch.setDevice(1)
   cutorch.setStream(3)
   -- device 1 stream 3 in use here
   cutorch.setDevice(2)
   -- device 2 stream 3 in use here
   ---
   0 is the default stream on the device.
*/
static int cutorch_setStream(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  int stream = (int) luaL_checknumber(L, 1);
  THCState_setStreamForCurrentDevice(state, stream);

  return 0;
}

/*
   Usage:
   n = cutorch.getStream()
   Returns the current user stream for all devices in use (as previously
   set via cutorch.setStream(n). 0 is the default stream on the device
   and is its initial value.
*/
static int cutorch_getStream(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  lua_pushnumber(L, THCState_getCurrentStreamIndex(state));

  return 1;
}

/*
   Usage:
   cutorch.setDefaultStream()
   Equivalent to cutorch.setStream(0).
*/
static int cutorch_setDefaultStream(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  THCState_setStreamForCurrentDevice(state, 0);

  return 0;
}

/*
   Usage:
   cutorch.streamWaitFor(waiterStream, {waitForStream1, ..., waitForStreamN})
   for streams on the current device. Creates a one-way barrier where
   waiterStream waits for waitForStream1-N to reach the current point.
*/
static int cutorch_streamWaitFor(lua_State *L)
{
  THCState *state = cutorch_getstate(L);

  int curDev = -1;
  THCudaCheck(cudaGetDevice(&curDev));

  /* Check that the waiting stream is in bounds; this will error out if not */
  int waitingId = (int) luaL_checknumber(L, 1);
  cudaStream_t streamWaiting =
    THCState_getDeviceStream(state, curDev, waitingId);

  /* Validate the streams that we are waiting on */
  int streams = checkAndCountListOfStreams(L, state, 2, curDev);

  if (streams < 1) {
    /* nothing to synchronize */
    return 0;
  }

  /* One-way dependency; streamWaiting will wait for the list of streams to
     wait on to complete execution of pending scheduled kernels/events */
  cudaEvent_t event;
  createSingleDeviceEvent(L, state, 2, curDev, &event);

  /* Then, wait on them */
  THCudaCheck(cudaStreamWaitEvent(streamWaiting, event, 0));
  THCudaCheck(cudaEventDestroy(event));

  return 0;
}

/*
   Usage:
   cutorch.streamWaitForMultiDevice(gpuWaiter, streamWaiter,
                                    {[gpu1]={stream1_1, ..., stream1_N},
                                    [gpuK]={streamK_1, ..., streamK_M}})
   with a specified GPU per each list of streams.
   Stream (gpuWaiter, streamWaiter) will wait on all of the other streams
   (gpu1, stream1_1), ..., (gpu1, stream1_N), ...,
   (gpuK, streamK_1), ..., (gpuK, streamK_M) to complete fully, as a one-way
   barrier only (only streamWaiter is blocked).
   The streams to wait on are bucketed per device. Equivalent to
   streamWaitFor() if only one GPU's streams are listed.
*/
static int cutorch_streamWaitForMultiDevice(lua_State *L)
{
  THCState *state = cutorch_getstate(L);

  int prevDev = -1;
  THCudaCheck(cudaGetDevice(&prevDev));

  /* Validate waiting (gpu, stream); this will error out if not */
  int gpuWaiter = (int) luaL_checknumber(L, 1) - 1;
  int streamWaiter = (int) luaL_checknumber(L, 2);
  cudaStream_t streamWaiting =
    THCState_getDeviceStream(state, gpuWaiter, streamWaiter);

  /* Validate and count set of {gpu={streams...}} we are waiting on */
  int gpus = 0;
  int streams = 0;
  checkAndCountListOfGPUStreamPairs(L, state, 3, &gpus, &streams);

  if (streams < 2) {
    /* nothing to synchronize together */
    return 0;
  }

  /*
     Events can only be recorded on the same device on which they are created.
     -For each GPU, create an event, and record that event on each stream given
     for that GPU.
     -For (gpuWaiter, streamWaiter), wait on all of the above events.
  */
  cudaEvent_t* events = (cudaEvent_t*) malloc(sizeof(cudaEvent_t) * gpus);

  /* First, create an event per GPU and record events for the specified stream
     on that GPU */
  createMultiDeviceEvents(L, state, 3, events);

  /* Then, wait on the events */
  THCudaCheck(cudaSetDevice(gpuWaiter));
  for (int i = 0; i < gpus; ++i) {
    THCudaCheck(cudaStreamWaitEvent(streamWaiting, events[i], 0));
  }

  /* Clean up events */
  for (int i = 0; i < gpus; ++i) {
    THCudaCheck(cudaEventDestroy(events[i]));
  }
  free(events);
  THCudaCheck(cudaSetDevice(prevDev));

  return 0;
}

/*
   Usage:
   cutorch.streamBarrier({stream1, stream2, ..., streamN})
   applies to streams for the current device. Creates a N-way barrier
   to synchronize all of the streams given
*/
static int cutorch_streamBarrier(lua_State *L)
{
  THCState *state = cutorch_getstate(L);

  int curDev = -1;
  THCudaCheck(cudaGetDevice(&curDev));

  int streams = checkAndCountListOfStreams(L, state, 1, curDev);

  if (streams < 2) {
    /* nothing to synchronize together */
    return 0;
  }

  /* Multi-way dependency (barrier); all streams must complete execution
     of pending scheduled kernels/events */
  cudaEvent_t event;

  /* First, create an event and record them for all streams */
  createSingleDeviceEvent(L, state, 1, curDev, &event);

  /* Then, wait on the event. Each stream is actually waiting on itself here
     too, but that's harmless and isn't worth weeding out. */
  waitSingleDeviceEvent(L, state, 1, curDev, event);
  THCudaCheck(cudaEventDestroy(event));

  return 0;
}

/* usage:
   cutorch.streamBarrierMultiDevice({[gpu1]={stream1_1, ..., stream1_N},
                                     [gpuK]={streamK_1, ..., streamK_M}})
   with a specified GPU per each list of streams.
   Each stream (gpu1, stream1_1), ..., (gpu1, stream1_N), ...,
               (gpuK, streamK_1), ..., (gpuK, streamK_M) will wait
   for all others to complete fully.
   Streams are bucketed per device. Equivalent to streamBarrier() if only
   one GPU is specified.
 */
static int cutorch_streamBarrierMultiDevice(lua_State *L)
{
  THCState *state = cutorch_getstate(L);

  int prevDev = -1;
  THCudaCheck(cudaGetDevice(&prevDev));

  /* Validate and count set of {gpu={streams...}} that are mutually waiting */
  int gpus = 0;
  int streams = 0;
  checkAndCountListOfGPUStreamPairs(L, state, 1, &gpus, &streams);

  if (streams < 2) {
    /* nothing to synchronize together */
    return 0;
  }

  /*
     Events can only be recorded on the same device on which they are created.
     -For each GPU, create an event, and record that event on each stream given
     for that GPU.
     -For each GPU, for each stream, wait on the event created by each other
     GPU.
  */
  cudaEvent_t* events = (cudaEvent_t*) malloc(sizeof(cudaEvent_t) * gpus);

  /* First, create an event per GPU and record events for the specified stream
     on that GPU */
  createMultiDeviceEvents(L, state, 1, events);

  /* Then, wait on the events. Each stream is actually waiting on itself here
     too, but that's harmless and isn't worth weeding out. */
  waitMultiDeviceEvents(L, state, 1, events, gpus);

  /* Clean up events */
  for (int i = 0; i < gpus; ++i) {
    THCudaCheck(cudaEventDestroy(events[i]));
  }
  free(events);
  THCudaCheck(cudaSetDevice(prevDev));

  return 0;
}

/*
   Usage:
   cutorch.streamSynchronize(n)
   For the current device, synchronizes with the given stream only
   (cudaStreamSynchronize).
   0 is the default stream on the device.
*/
static int cutorch_streamSynchronize(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  int streamId = (int) luaL_checknumber(L, 1);

  int curDev = -1;
  THCudaCheck(cudaGetDevice(&curDev));

  /* This also validates the stream */
  cudaStream_t stream = THCState_getDeviceStream(state, curDev, streamId);
  THCudaCheck(cudaStreamSynchronize(stream));

  return 0;
}

static int cutorch_getDevice(lua_State *L)
{
  int device;
  THCudaCheck(cudaGetDevice(&device));
  device++;
  lua_pushnumber(L, device);
  return 1;
}

static int cutorch_deviceReset(lua_State *L)
{
  printf("WARNING: cutorch.deviceReset has been depreceated."
	 " Just remove the call from your code.\n");
  return 0;
}

static int cutorch_getDeviceCount(lua_State *L)
{
  int ndevice;
  THCudaCheck(cudaGetDeviceCount(&ndevice));
  lua_pushnumber(L, ndevice);
  return 1;
}

static int cutorch_getMemoryUsage(lua_State *L) {
  size_t freeBytes = 0;
  size_t totalBytes = 0;
  int curDevice;
  THCudaCheck(cudaGetDevice(&curDevice));

  int device = luaL_optint(L, 1, -10);
  if (device == -10) { /* no argument passed, current device mem usage */
    THCudaCheck(cudaMemGetInfo(&freeBytes, &totalBytes));
  } else { /* argument was given, particular device's memory usage */
    THCudaCheck(cudaSetDevice(device-1)); /* zero indexed */
    THCudaCheck(cudaMemGetInfo(&freeBytes, &totalBytes));
    THCudaCheck(cudaSetDevice(curDevice));
  }
  lua_pushnumber(L, freeBytes);
  lua_pushnumber(L, totalBytes);
  return 2;
}

static int cutorch_setDevice(lua_State *L)
{
  THCState *state = cutorch_getstate(L);
  int device = (int)luaL_checknumber(L, 1)-1;
  THCudaCheck(cudaSetDevice(device));
  THCRandom_setGenerator(state, device);
  THCudaBlas_setHandle(state, device);

  /* The stream is per device, so update the stream as well */
  THCState_setStream(state, device, THCState_getCurrentStreamIndex(state));

  return 0;
}

#define SET_DEVN_PROP(NAME) \
  lua_pushnumber(L, prop.NAME); \
  lua_setfield(L, -2, #NAME);

static int cutorch_getDeviceProperties(lua_State *L)
{
  struct cudaDeviceProp prop;
  int device = (int)luaL_checknumber(L, 1)-1;

  THCudaCheck(cudaGetDeviceProperties(&prop, device));
  lua_newtable(L);
  SET_DEVN_PROP(canMapHostMemory);
  SET_DEVN_PROP(clockRate);
  SET_DEVN_PROP(computeMode);
  SET_DEVN_PROP(deviceOverlap);
  SET_DEVN_PROP(integrated);
  SET_DEVN_PROP(kernelExecTimeoutEnabled);
  SET_DEVN_PROP(major);
  SET_DEVN_PROP(maxThreadsPerBlock);
  SET_DEVN_PROP(memPitch);
  SET_DEVN_PROP(minor);
  SET_DEVN_PROP(multiProcessorCount);
  SET_DEVN_PROP(regsPerBlock);
  SET_DEVN_PROP(sharedMemPerBlock);
  SET_DEVN_PROP(textureAlignment);
  SET_DEVN_PROP(totalConstMem);
  SET_DEVN_PROP(totalGlobalMem);
  SET_DEVN_PROP(warpSize);
  SET_DEVN_PROP(pciBusID);
  SET_DEVN_PROP(pciDeviceID);
  SET_DEVN_PROP(pciDomainID);
  SET_DEVN_PROP(maxTexture1D);
  SET_DEVN_PROP(maxTexture1DLinear);

  size_t freeMem;
  THCudaCheck(cudaMemGetInfo (&freeMem, NULL));
  lua_pushnumber(L, freeMem);
  lua_setfield(L, -2, "freeGlobalMem");

  lua_pushstring(L, prop.name);
  lua_setfield(L, -2, "name");

  return 1;
}

static int cutorch_seed(lua_State *L)
{
  unsigned long seed = THCRandom_seed(cutorch_getstate(L));
  lua_pushnumber(L, seed);
  return 1;
}

static int cutorch_seedAll(lua_State *L)
{
  unsigned long seed = THCRandom_seedAll(cutorch_getstate(L));
  lua_pushnumber(L, seed);
  return 1;
}

static int cutorch_initialSeed(lua_State *L)
{
  unsigned long seed = THCRandom_initialSeed(cutorch_getstate(L));
  lua_pushnumber(L, seed);
  return 1;
}

static int cutorch_manualSeed(lua_State *L)
{
  unsigned long seed = luaL_checknumber(L, 1);
  THCRandom_manualSeed(cutorch_getstate(L), seed);
  return 0;
}

static int cutorch_manualSeedAll(lua_State* L)
{
  unsigned long seed = luaL_checknumber(L, 1);
  THCRandom_manualSeedAll(cutorch_getstate(L), seed);
  return 0;
}

static int cutorch_getRNGState(lua_State *L)
{
  THByteTensor* t = THByteTensor_new();
  THCRandom_getRNGState(cutorch_getstate(L), t);
  luaT_pushudata(L, t, "torch.ByteTensor");
  return 1;
}

static int cutorch_setRNGState(lua_State *L)
{
  THByteTensor* t = luaT_checkudata(L, 1, "torch.ByteTensor");
  THCRandom_setRNGState(cutorch_getstate(L), t);
  return 0;
}

static int cutorch_getState(lua_State *L)
{
  lua_getglobal(L, "cutorch");
  lua_getfield(L, -1, "_state");
  lua_remove(L, -2);
  return 1;
}

static const struct luaL_Reg cutorch_stuff__ [] = {
  {"synchronize", cutorch_synchronize},
  {"reserveStreams", cutorch_reserveStreams},
  {"getNumStreams", cutorch_getNumStreams},
  {"setStream", cutorch_setStream},
  {"getStream", cutorch_getStream},
  {"setDefaultStream", cutorch_setDefaultStream},
  {"streamWaitFor", cutorch_streamWaitFor},
  {"streamWaitForMultiDevice", cutorch_streamWaitForMultiDevice},
  {"streamBarrier", cutorch_streamBarrier},
  {"streamBarrierMultiDevice", cutorch_streamBarrierMultiDevice},
  {"streamSynchronize", cutorch_streamSynchronize},
  {"getDevice", cutorch_getDevice},
  {"deviceReset", cutorch_deviceReset},
  {"getDeviceCount", cutorch_getDeviceCount},
  {"getDeviceProperties", cutorch_getDeviceProperties},
  {"getMemoryUsage", cutorch_getMemoryUsage},
  {"setDevice", cutorch_setDevice},
  {"seed", cutorch_seed},
  {"seedAll", cutorch_seedAll},
  {"initialSeed", cutorch_initialSeed},
  {"manualSeed", cutorch_manualSeed},
  {"manualSeedAll", cutorch_manualSeedAll},
  {"getRNGState", cutorch_getRNGState},
  {"setRNGState", cutorch_setRNGState},
  {"getState", cutorch_getState},
  {NULL, NULL}
};

LUA_EXTERNC DLL_EXPORT int luaopen_libcutorch(lua_State *L);

int luaopen_libcutorch(lua_State *L)
{
  lua_newtable(L);
  luaL_register(L, NULL, cutorch_stuff__);

  THCState* state = (THCState*)malloc(sizeof(THCState));
  THCudaInit(state);

  cutorch_CudaStorage_init(L);
  cutorch_CudaTensor_init(L);
  cutorch_CudaTensorMath_init(L);
  cutorch_CudaTensorOperator_init(L);

  /* Store state in cutorch table. */
  lua_pushlightuserdata(L, state);
  lua_setfield(L, -2, "_state");

  return 1;
}
