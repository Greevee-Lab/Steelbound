/* vr_openxr.h - W134 VR-PROTO: minimale OpenXR-1.0-Deklarationen (eigene Arbeit).
 *
 * Kein OpenXR-SDK im Projekt (keine Downloads, keine fremden Header). Hier
 * stehen nur die Typen, Konstanten und Funktionszeiger, die der Prototyp
 * (vr.c) und die Test-Runtime (tests/vr_mock_runtime.c) brauchen. Werte und
 * Struktur-Layouts folgen der OpenXR-1.0-Spezifikation (Kapitel "Fundamentals",
 * "Instance", "System", "Session", "Spaces", "View Configurations",
 * "Swapchain", "Frame Submission", "Compositing", Erweiterung
 * XR_KHR_D3D11_enable) und dem Loader-Runtime-Interface der Loader-
 * Spezifikation ("Runtime Interface Negotiation"). Reihenfolge der Felder =
 * Spezifikation; Ausrichtung = natuerliche MSVC-Ausrichtung (wie jede
 * C-Runtime-DLL fuer dieselbe Plattform).
 *
 * Wichtig fuer diesen 32-bit-Prozess: OpenXR-Handles sind auf Plattformen mit
 * 32-bit-Zeigern 64-bit-Ganzzahlen (XR_DEFINE_HANDLE), Aufrufkonvention der
 * API-Funktionen ist unter Windows __stdcall (XRAPI_CALL). */
#ifndef VR_OPENXR_H
#define VR_OPENXR_H
#include <stdint.h>
#include <stddef.h>

#define XRAPI_CALL __stdcall
#define XRAPI_PTR XRAPI_CALL

/* ---- Grundtypen (Spez. 2.x "Fundamentals") ---- */
typedef uint64_t XrVersion;
typedef uint64_t XrFlags64;
typedef int64_t XrTime;
typedef int64_t XrDuration;
typedef uint32_t XrBool32;
typedef uint64_t XrSystemId;
typedef uint64_t XrPath;
typedef int32_t XrResult;
#define XR_MAKE_VERSION(maj, min, pat) \
    ((((uint64_t)(maj) & 0xffffULL) << 48) | (((uint64_t)(min) & 0xffffULL) << 32) | ((uint64_t)(pat) & 0xffffffffULL))
#define XR_VERSION_MAJOR(v) ((uint16_t)(((uint64_t)(v) >> 48) & 0xffffULL))
#define XR_VERSION_MINOR(v) ((uint16_t)(((uint64_t)(v) >> 32) & 0xffffULL))
#define XR_TRUE 1u
#define XR_FALSE 0u
#define XR_NULL_HANDLE 0ull
#define XR_NO_DURATION 0
#define XR_INFINITE_DURATION 0x7fffffffffffffffLL

/* Handles: 32-bit-Zeiger -> uint64_t (XR_DEFINE_HANDLE) */
typedef uint64_t XrInstance;
typedef uint64_t XrSession;
typedef uint64_t XrSpace;
typedef uint64_t XrSwapchain;

/* ---- Ergebniscodes (Auszug, Spez. "XrResult") ---- */
#define XR_SUCCESS 0
#define XR_TIMEOUT_EXPIRED 1
#define XR_SESSION_LOSS_PENDING 3
#define XR_EVENT_UNAVAILABLE 4
#define XR_SESSION_NOT_FOCUSED 8
#define XR_FRAME_DISCARDED 9
#define XR_ERROR_VALIDATION_FAILURE (-1)
#define XR_ERROR_RUNTIME_FAILURE (-2)
#define XR_ERROR_OUT_OF_MEMORY (-3)
#define XR_ERROR_API_VERSION_UNSUPPORTED (-4)
#define XR_ERROR_INITIALIZATION_FAILED (-6)
#define XR_ERROR_FUNCTION_UNSUPPORTED (-7)
#define XR_ERROR_FEATURE_UNSUPPORTED (-8)
#define XR_ERROR_EXTENSION_NOT_PRESENT (-9)
#define XR_ERROR_LIMIT_REACHED (-10)
#define XR_ERROR_SIZE_INSUFFICIENT (-11)
#define XR_ERROR_HANDLE_INVALID (-12)
#define XR_ERROR_INSTANCE_LOST (-13)
#define XR_ERROR_SESSION_RUNNING (-14)
#define XR_ERROR_SESSION_NOT_RUNNING (-16)
#define XR_ERROR_SESSION_LOST (-17)
#define XR_ERROR_SYSTEM_INVALID (-18)
#define XR_ERROR_SESSION_NOT_READY (-28)
#define XR_ERROR_SESSION_NOT_STOPPING (-29)
#define XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35)
#define XR_ERROR_CALL_ORDER_INVALID (-37)
#define XR_ERROR_GRAPHICS_DEVICE_INVALID (-38)
#define XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED (-41)
#define XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING (-50)
#define XR_SUCCEEDED(r) ((r) >= 0)
#define XR_FAILED(r) ((r) < 0)

/* ---- XrStructureType (Auszug; Kern 1.0 + XR_KHR_D3D11_enable, Erweiterung Nr. 28
 *      -> 1000000000 + (28-1)*1000 + n) ---- */
typedef enum XrStructureType {
    XR_TYPE_UNKNOWN = 0,
    XR_TYPE_EXTENSION_PROPERTIES = 2,
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_SYSTEM_PROPERTIES = 5,
    XR_TYPE_VIEW_LOCATE_INFO = 6,
    XR_TYPE_VIEW = 7,
    XR_TYPE_SESSION_CREATE_INFO = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 9,
    XR_TYPE_SESSION_BEGIN_INFO = 10,
    XR_TYPE_VIEW_STATE = 11,
    XR_TYPE_FRAME_END_INFO = 12,
    XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING = 17,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_INSTANCE_PROPERTIES = 32,
    XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING = 40,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 41,
    XR_TYPE_FRAME_STATE = 44,
    XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    XR_TYPE_EVENT_DATA_EVENTS_LOST = 49,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_GRAPHICS_BINDING_D3D11_KHR = 1000027000,
    XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR = 1000027001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR = 1000027002,
    XR_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrStructureType;

typedef enum XrFormFactor { XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1, XR_FORM_FACTOR_MAX_ENUM = 0x7FFFFFFF } XrFormFactor;
typedef enum XrViewConfigurationType {
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO = 1,
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2,
    XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrViewConfigurationType;
typedef enum XrEnvironmentBlendMode {
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1,
    XR_ENVIRONMENT_BLEND_MODE_ADDITIVE = 2,
    XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND = 3,
    XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM = 0x7FFFFFFF
} XrEnvironmentBlendMode;
typedef enum XrReferenceSpaceType {
    XR_REFERENCE_SPACE_TYPE_VIEW = 1,
    XR_REFERENCE_SPACE_TYPE_LOCAL = 2,
    XR_REFERENCE_SPACE_TYPE_STAGE = 3,
    XR_REFERENCE_SPACE_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrReferenceSpaceType;
typedef enum XrSessionState {
    XR_SESSION_STATE_UNKNOWN = 0,
    XR_SESSION_STATE_IDLE = 1,
    XR_SESSION_STATE_READY = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE = 4,
    XR_SESSION_STATE_FOCUSED = 5,
    XR_SESSION_STATE_STOPPING = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING = 8,
    XR_SESSION_STATE_MAX_ENUM = 0x7FFFFFFF
} XrSessionState;

#define XR_MAX_EXTENSION_NAME_SIZE 128
#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE 128
#define XR_MAX_RUNTIME_NAME_SIZE 128
#define XR_MAX_SYSTEM_NAME_SIZE 256

/* Swapchain-Nutzung (XrSwapchainUsageFlags) */
#define XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT 0x00000001ull
#define XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT 0x00000010ull
#define XR_SWAPCHAIN_USAGE_SAMPLED_BIT 0x00000020ull
/* XrViewStateFlags */
#define XR_VIEW_STATE_ORIENTATION_VALID_BIT 0x00000001ull
#define XR_VIEW_STATE_POSITION_VALID_BIT 0x00000002ull

/* ---- Strukturen ---- */
typedef struct XrBaseInStructure { XrStructureType type; const void *next; } XrBaseInStructure;

typedef struct XrExtensionProperties {
    XrStructureType type;
    void *next;
    char extensionName[XR_MAX_EXTENSION_NAME_SIZE];
    uint32_t extensionVersion;
} XrExtensionProperties;

typedef struct XrApplicationInfo {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    uint32_t applicationVersion;
    char engineName[XR_MAX_ENGINE_NAME_SIZE];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct XrInstanceCreateInfo {
    XrStructureType type;
    const void *next;
    XrFlags64 createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char *const *enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct XrInstanceProperties {
    XrStructureType type;
    void *next;
    XrVersion runtimeVersion;
    char runtimeName[XR_MAX_RUNTIME_NAME_SIZE];
} XrInstanceProperties;

typedef struct XrSystemGetInfo {
    XrStructureType type;
    const void *next;
    XrFormFactor formFactor;
} XrSystemGetInfo;

typedef struct XrSystemGraphicsProperties {
    uint32_t maxSwapchainImageHeight;
    uint32_t maxSwapchainImageWidth;
    uint32_t maxLayerCount;
} XrSystemGraphicsProperties;
typedef struct XrSystemTrackingProperties {
    XrBool32 orientationTracking;
    XrBool32 positionTracking;
} XrSystemTrackingProperties;
typedef struct XrSystemProperties {
    XrStructureType type;
    void *next;
    XrSystemId systemId;
    uint32_t vendorId;
    char systemName[XR_MAX_SYSTEM_NAME_SIZE];
    XrSystemGraphicsProperties graphicsProperties;
    XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;

typedef struct XrViewConfigurationView {
    XrStructureType type;
    void *next;
    uint32_t recommendedImageRectWidth;
    uint32_t maxImageRectWidth;
    uint32_t recommendedImageRectHeight;
    uint32_t maxImageRectHeight;
    uint32_t recommendedSwapchainSampleCount;
    uint32_t maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct XrVector3f { float x, y, z; } XrVector3f;
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrPosef { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef struct XrOffset2Di { int32_t x, y; } XrOffset2Di;
typedef struct XrExtent2Di { int32_t width, height; } XrExtent2Di;
typedef struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;

typedef struct XrSessionCreateInfo {
    XrStructureType type;
    const void *next;
    XrFlags64 createFlags;
    XrSystemId systemId;
} XrSessionCreateInfo;

typedef struct XrSessionBeginInfo {
    XrStructureType type;
    const void *next;
    XrViewConfigurationType primaryViewConfigurationType;
} XrSessionBeginInfo;

typedef struct XrReferenceSpaceCreateInfo {
    XrStructureType type;
    const void *next;
    XrReferenceSpaceType referenceSpaceType;
    XrPosef poseInReferenceSpace;
} XrReferenceSpaceCreateInfo;

typedef struct XrSwapchainCreateInfo {
    XrStructureType type;
    const void *next;
    XrFlags64 createFlags;
    XrFlags64 usageFlags;
    int64_t format;
    uint32_t sampleCount;
    uint32_t width;
    uint32_t height;
    uint32_t faceCount;
    uint32_t arraySize;
    uint32_t mipCount;
} XrSwapchainCreateInfo;

typedef struct XrSwapchainImageBaseHeader { XrStructureType type; void *next; } XrSwapchainImageBaseHeader;
typedef struct XrSwapchainImageAcquireInfo { XrStructureType type; const void *next; } XrSwapchainImageAcquireInfo;
typedef struct XrSwapchainImageWaitInfo { XrStructureType type; const void *next; XrDuration timeout; } XrSwapchainImageWaitInfo;
typedef struct XrSwapchainImageReleaseInfo { XrStructureType type; const void *next; } XrSwapchainImageReleaseInfo;

typedef struct XrFrameWaitInfo { XrStructureType type; const void *next; } XrFrameWaitInfo;
typedef struct XrFrameState {
    XrStructureType type;
    void *next;
    XrTime predictedDisplayTime;
    XrDuration predictedDisplayPeriod;
    XrBool32 shouldRender;
} XrFrameState;
typedef struct XrFrameBeginInfo { XrStructureType type; const void *next; } XrFrameBeginInfo;

typedef struct XrCompositionLayerBaseHeader {
    XrStructureType type;
    const void *next;
    XrFlags64 layerFlags;
    XrSpace space;
} XrCompositionLayerBaseHeader;

typedef struct XrFrameEndInfo {
    XrStructureType type;
    const void *next;
    XrTime displayTime;
    XrEnvironmentBlendMode environmentBlendMode;
    uint32_t layerCount;
    const XrCompositionLayerBaseHeader *const *layers;
} XrFrameEndInfo;

typedef struct XrViewLocateInfo {
    XrStructureType type;
    const void *next;
    XrViewConfigurationType viewConfigurationType;
    XrTime displayTime;
    XrSpace space;
} XrViewLocateInfo;
typedef struct XrViewState { XrStructureType type; void *next; XrFlags64 viewStateFlags; } XrViewState;
typedef struct XrView { XrStructureType type; void *next; XrPosef pose; XrFovf fov; } XrView;

typedef struct XrSwapchainSubImage {
    XrSwapchain swapchain;
    XrRect2Di imageRect;
    uint32_t imageArrayIndex;
} XrSwapchainSubImage;
typedef struct XrCompositionLayerProjectionView {
    XrStructureType type;
    const void *next;
    XrPosef pose;
    XrFovf fov;
    XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;
typedef struct XrCompositionLayerProjection {
    XrStructureType type;
    const void *next;
    XrFlags64 layerFlags;
    XrSpace space;
    uint32_t viewCount;
    const XrCompositionLayerProjectionView *views;
} XrCompositionLayerProjection;

typedef struct XrEventDataBuffer { XrStructureType type; const void *next; uint8_t varying[4000]; } XrEventDataBuffer;
typedef struct XrEventDataSessionStateChanged {
    XrStructureType type;
    const void *next;
    XrSession session;
    XrSessionState state;
    XrTime time;
} XrEventDataSessionStateChanged;

/* ---- XR_KHR_D3D11_enable (Erweiterung Nr. 28) ---- */
#define XR_KHR_D3D11_ENABLE_EXTENSION_NAME "XR_KHR_D3D11_enable"
struct ID3D11Device;
struct ID3D11Texture2D;
typedef struct XrGraphicsBindingD3D11KHR {
    XrStructureType type;
    const void *next;
    struct ID3D11Device *device;
} XrGraphicsBindingD3D11KHR;
typedef struct XrSwapchainImageD3D11KHR {
    XrStructureType type;
    void *next;
    struct ID3D11Texture2D *texture;
} XrSwapchainImageD3D11KHR;
typedef struct XrLuidStruct { uint32_t LowPart; int32_t HighPart; } XrLuidStruct; /* = Windows LUID */
typedef struct XrGraphicsRequirementsD3D11KHR {
    XrStructureType type;
    void *next;
    XrLuidStruct adapterLuid;
    int32_t minFeatureLevel; /* D3D_FEATURE_LEVEL */
} XrGraphicsRequirementsD3D11KHR;

/* ---- Funktionszeiger ---- */
typedef void (XRAPI_PTR *PFN_xrVoidFunction)(void);
typedef XrResult (XRAPI_PTR *PFN_xrGetInstanceProcAddr)(XrInstance, const char *, PFN_xrVoidFunction *);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateInstanceExtensionProperties)(const char *, uint32_t, uint32_t *, XrExtensionProperties *);
typedef XrResult (XRAPI_PTR *PFN_xrCreateInstance)(const XrInstanceCreateInfo *, XrInstance *);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyInstance)(XrInstance);
typedef XrResult (XRAPI_PTR *PFN_xrGetInstanceProperties)(XrInstance, XrInstanceProperties *);
typedef XrResult (XRAPI_PTR *PFN_xrPollEvent)(XrInstance, XrEventDataBuffer *);
typedef XrResult (XRAPI_PTR *PFN_xrGetSystem)(XrInstance, const XrSystemGetInfo *, XrSystemId *);
typedef XrResult (XRAPI_PTR *PFN_xrGetSystemProperties)(XrInstance, XrSystemId, XrSystemProperties *);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateViewConfigurationViews)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t *, XrViewConfigurationView *);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateEnvironmentBlendModes)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t *, XrEnvironmentBlendMode *);
typedef XrResult (XRAPI_PTR *PFN_xrCreateSession)(XrInstance, const XrSessionCreateInfo *, XrSession *);
typedef XrResult (XRAPI_PTR *PFN_xrDestroySession)(XrSession);
typedef XrResult (XRAPI_PTR *PFN_xrBeginSession)(XrSession, const XrSessionBeginInfo *);
typedef XrResult (XRAPI_PTR *PFN_xrEndSession)(XrSession);
typedef XrResult (XRAPI_PTR *PFN_xrRequestExitSession)(XrSession);
typedef XrResult (XRAPI_PTR *PFN_xrCreateReferenceSpace)(XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *);
typedef XrResult (XRAPI_PTR *PFN_xrDestroySpace)(XrSpace);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t *, int64_t *);
typedef XrResult (XRAPI_PTR *PFN_xrCreateSwapchain)(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *);
typedef XrResult (XRAPI_PTR *PFN_xrDestroySwapchain)(XrSwapchain);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateSwapchainImages)(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireSwapchainImage)(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *);
typedef XrResult (XRAPI_PTR *PFN_xrWaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo *);
typedef XrResult (XRAPI_PTR *PFN_xrReleaseSwapchainImage)(XrSwapchain, const XrSwapchainImageReleaseInfo *);
typedef XrResult (XRAPI_PTR *PFN_xrWaitFrame)(XrSession, const XrFrameWaitInfo *, XrFrameState *);
typedef XrResult (XRAPI_PTR *PFN_xrBeginFrame)(XrSession, const XrFrameBeginInfo *);
typedef XrResult (XRAPI_PTR *PFN_xrEndFrame)(XrSession, const XrFrameEndInfo *);
typedef XrResult (XRAPI_PTR *PFN_xrLocateViews)(XrSession, const XrViewLocateInfo *, XrViewState *, uint32_t, uint32_t *, XrView *);
typedef XrResult (XRAPI_PTR *PFN_xrGetD3D11GraphicsRequirementsKHR)(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR *);

/* ---- XR_FB_display_refresh_rate (Erweiterung Nr. 102, W183 VR-FIX3; nach Spezifikation) ---- */
#define XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME "XR_FB_display_refresh_rate"
#define XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB ((XrStructureType)1000101000)
#define XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB (-1000101000)
typedef struct XrEventDataDisplayRefreshRateChangedFB {
    XrStructureType type;
    const void *next;
    float fromDisplayRefreshRate;
    float toDisplayRefreshRate;
} XrEventDataDisplayRefreshRateChangedFB;
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateDisplayRefreshRatesFB)(XrSession, uint32_t, uint32_t *, float *);
typedef XrResult (XRAPI_PTR *PFN_xrGetDisplayRefreshRateFB)(XrSession, float *);
typedef XrResult (XRAPI_PTR *PFN_xrRequestDisplayRefreshRateFB)(XrSession, float);

/* ---- Loader <-> Runtime (Loader-Spezifikation, "Runtime Interface Negotiation") ---- */
typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCTS_MAX_ENUM = 0x7FFFFFFF
} XrLoaderInterfaceStructs;
#define XR_LOADER_INFO_STRUCT_VERSION 1
#define XR_RUNTIME_INFO_STRUCT_VERSION 1
#define XR_CURRENT_LOADER_RUNTIME_VERSION 1

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType; /* XR_LOADER_INTERFACE_STRUCT_LOADER_INFO */
    uint32_t structVersion;              /* XR_LOADER_INFO_STRUCT_VERSION */
    size_t structSize;                   /* sizeof(XrNegotiateLoaderInfo) */
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType; /* XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST */
    uint32_t structVersion;              /* XR_RUNTIME_INFO_STRUCT_VERSION */
    size_t structSize;                   /* sizeof(XrNegotiateRuntimeRequest) */
    uint32_t runtimeInterfaceVersion;    /* von der Runtime gesetzt */
    XrVersion runtimeApiVersion;         /* von der Runtime gesetzt */
    PFN_xrGetInstanceProcAddr getInstanceProcAddr; /* von der Runtime gesetzt */
} XrNegotiateRuntimeRequest;

typedef XrResult (XRAPI_PTR *PFN_xrNegotiateLoaderRuntimeInterface)(const XrNegotiateLoaderInfo *, XrNegotiateRuntimeRequest *);

#endif /* VR_OPENXR_H */
