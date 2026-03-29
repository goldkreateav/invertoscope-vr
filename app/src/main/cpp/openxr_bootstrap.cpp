#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* TAG = "InvertoscopeXR";
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct EyeTransform {
    float rotationDegrees = 0.0f;
    bool mirrorX = false;
    bool mirrorY = false;
};

struct SwapchainBundle {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> framebuffers;
};

class InvertoscopeRuntime {
public:
    std::string Start(JNIEnv* env, jobject activity, jobject callback) {
        if (running_) {
            return "OpenXR уже запущен";
        }
        env->GetJavaVM(&javaVm_);
        activity_ = env->NewGlobalRef(activity);
        callback_ = env->NewGlobalRef(callback);

        jclass callbackClass = env->GetObjectClass(callback);
        callbackMethod_ = env->GetMethodID(callbackClass, "onNativeUpdateCameraTextures", "()Z");
        env->DeleteLocalRef(callbackClass);

        running_ = true;
        paused_ = false;
        inputEnabled_ = true;
        initDone_ = false;
        initStatus_ = "Инициализация OpenXR...";
        renderThread_ = std::thread(&InvertoscopeRuntime::RenderLoop, this);

        std::unique_lock<std::mutex> lock(initMutex_);
        initCv_.wait_for(lock, std::chrono::seconds(8), [this]() { return initDone_; });
        return initStatus_;
    }

    void Stop(JNIEnv* env) {
        running_ = false;
        paused_ = false;
        if (renderThread_.joinable()) {
            renderThread_.join();
        }
        if (callback_) {
            env->DeleteGlobalRef(callback_);
            callback_ = nullptr;
        }
        if (activity_) {
            env->DeleteGlobalRef(activity_);
            activity_ = nullptr;
        }
    }

    void OnResume() { paused_ = false; }
    void OnPause() { paused_ = true; }

    int GetCameraTextureId(int eye) const {
        if (eye < 0 || eye > 1) return 0;
        return static_cast<int>(cameraTextureIds_[eye]);
    }

    void SetCameraTextureMatrix(int eye, const float* matrix16) {
        if (eye < 0 || eye > 1 || matrix16 == nullptr) return;
        std::lock_guard<std::mutex> lock(dataMutex_);
        std::memcpy(textureMatrices_[eye].data(), matrix16, sizeof(float) * 16);
    }

private:
    bool CheckXr(XrResult result, const char* op, bool fatal = true) {
        if (result == XR_SUCCESS) return true;
        std::ostringstream oss;
        oss << op << " failed, XrResult=" << static_cast<int>(result);
        LOGE("%s", oss.str().c_str());
        if (fatal) {
            initStatus_ = "OpenXR ошибка: " + std::string(op);
        }
        return false;
    }

    int64_t ChooseSwapchainFormat() {
        uint32_t formatCount = 0;
        if (!CheckXr(xrEnumerateSwapchainFormats_(session_, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats(count)")) {
            return 0;
        }
        if (formatCount == 0) {
            initStatus_ = "OpenXR: runtime не вернул swapchain format";
            return 0;
        }

        std::vector<int64_t> formats(formatCount, 0);
        if (!CheckXr(xrEnumerateSwapchainFormats_(session_, formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats(data)")) {
            return 0;
        }

        const std::array<int64_t, 3> preferred = {
            static_cast<int64_t>(GL_SRGB8_ALPHA8),
            static_cast<int64_t>(GL_RGBA8),
            static_cast<int64_t>(GL_RGBA16F)
        };

        for (int64_t candidate : preferred) {
            for (int64_t f : formats) {
                if (f == candidate) {
                    LOGI("Selected swapchain format: %lld", static_cast<long long>(candidate));
                    return candidate;
                }
            }
        }

        std::ostringstream oss;
        oss << "OpenXR: нет подходящего swapchain format. Доступно: ";
        for (size_t i = 0; i < formats.size(); ++i) {
            oss << formats[i] << (i + 1 < formats.size() ? ", " : "");
        }
        initStatus_ = oss.str();
        LOGE("%s", initStatus_.c_str());
        return 0;
    }

    void RenderLoop() {
        JNIEnv* env = nullptr;
        javaVm_->AttachCurrentThread(&env, nullptr);

        bool ok = InitializeRuntime();
        {
            std::lock_guard<std::mutex> lock(initMutex_);
            initDone_ = true;
        }
        initCv_.notify_all();

        if (!ok) {
            javaVm_->DetachCurrentThread();
            return;
        }

        while (running_) {
            PollEvents();

            if (!sessionRunning_ || paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            if (xrWaitFrame_(session_, &waitInfo, &frameState) != XR_SUCCESS) {
                continue;
            }

            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            if (xrBeginFrame_(session_, &beginInfo) != XR_SUCCESS) {
                continue;
            }

            if (!frameState.shouldRender) {
                EndFrameNoLayers(frameState.predictedDisplayTime);
                continue;
            }

            UpdateCameraTextures(env);
            SyncInput(frameState.predictedDisplayPeriod);

            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = appSpace_;

            XrViewState viewState{XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 0;
            if (xrLocateViews_(session_, &locateInfo, &viewState, static_cast<uint32_t>(views_.size()), &viewCount, views_.data()) != XR_SUCCESS) {
                EndFrameNoLayers(frameState.predictedDisplayTime);
                continue;
            }

            RenderStereoViews(viewCount);
            EndFrameProjection(frameState.predictedDisplayTime, viewCount);
        }

        ShutdownRuntime();
        javaVm_->DetachCurrentThread();
    }

    bool InitializeRuntime() {
        InitIdentityMatrices();
        loaderHandle_ = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
        if (!loaderHandle_) {
            initStatus_ = "OpenXR: загрузчик не найден";
            return false;
        }
        xrGetInstanceProcAddr_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loaderHandle_, "xrGetInstanceProcAddr"));
        if (!xrGetInstanceProcAddr_) {
            initStatus_ = "OpenXR: xrGetInstanceProcAddr недоступен";
            return false;
        }
        if (!LoadGlobalFunctions() || !CreateInstance() || !LoadInstanceFunctions() || !GetSystem() || !CreateEglContext() || !CreateSession() ||
            !CreateReferenceSpace() || !CreateSwapchains() || !CreateProgramAndGeometry() || !CreateActions()) {
            return false;
        }
        CreateCameraTextures();
        initStatus_ = "OpenXR иммерсивный режим запущен";
        return true;
    }

    bool LoadFn(XrInstance inst, const char* name, PFN_xrVoidFunction* outFn) {
        return xrGetInstanceProcAddr_(inst, name, outFn) == XR_SUCCESS && *outFn != nullptr;
    }

    bool LoadGlobalFunctions() {
        return LoadFn(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance_)) &&
               LoadFn(XR_NULL_HANDLE, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance_)) &&
               LoadFn(XR_NULL_HANDLE, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystem_));
    }

    bool LoadInstanceFunctions() {
        return LoadFn(instance_, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateSession_)) &&
               LoadFn(instance_, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroySession_)) &&
               LoadFn(instance_, "xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateReferenceSpace_)) &&
               LoadFn(instance_, "xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroySpace_)) &&
               LoadFn(instance_, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateViewConfigurationViews_)) &&
               LoadFn(instance_, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateSwapchain_)) &&
               LoadFn(instance_, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroySwapchain_)) &&
               LoadFn(instance_, "xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateSwapchainFormats_)) &&
               LoadFn(instance_, "xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateSwapchainImages_)) &&
               LoadFn(instance_, "xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&xrAcquireSwapchainImage_)) &&
               LoadFn(instance_, "xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&xrWaitSwapchainImage_)) &&
               LoadFn(instance_, "xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&xrReleaseSwapchainImage_)) &&
               LoadFn(instance_, "xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrWaitFrame_)) &&
               LoadFn(instance_, "xrBeginFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrBeginFrame_)) &&
               LoadFn(instance_, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrEndFrame_)) &&
               LoadFn(instance_, "xrLocateViews", reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateViews_)) &&
               LoadFn(instance_, "xrPollEvent", reinterpret_cast<PFN_xrVoidFunction*>(&xrPollEvent_)) &&
               LoadFn(instance_, "xrBeginSession", reinterpret_cast<PFN_xrVoidFunction*>(&xrBeginSession_)) &&
               LoadFn(instance_, "xrEndSession", reinterpret_cast<PFN_xrVoidFunction*>(&xrEndSession_)) &&
               LoadFn(instance_, "xrStringToPath", reinterpret_cast<PFN_xrVoidFunction*>(&xrStringToPath_)) &&
               LoadFn(instance_, "xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateActionSet_)) &&
               LoadFn(instance_, "xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyActionSet_)) &&
               LoadFn(instance_, "xrCreateAction", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateAction_)) &&
               LoadFn(instance_, "xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyAction_)) &&
               LoadFn(instance_, "xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction*>(&xrSuggestInteractionProfileBindings_)) &&
               LoadFn(instance_, "xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction*>(&xrAttachSessionActionSets_)) &&
               LoadFn(instance_, "xrSyncActions", reinterpret_cast<PFN_xrVoidFunction*>(&xrSyncActions_)) &&
               LoadFn(instance_, "xrGetActionStateBoolean", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetActionStateBoolean_)) &&
               LoadFn(instance_, "xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetActionStateFloat_)) &&
               LoadFn(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetOpenGLESGraphicsRequirementsKHR_));
    }

    bool CreateInstance() {
        const char* extensions[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = javaVm_;
        androidInfo.applicationActivity = activity_;

        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        info.next = &androidInfo;
        std::strncpy(info.applicationInfo.applicationName, "InvertoscopeQuest", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(info.applicationInfo.engineName, "NativeOpenXR", XR_MAX_ENGINE_NAME_SIZE - 1);
        info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
        info.enabledExtensionNames = extensions;

        if (xrCreateInstance_(&info, &instance_) != XR_SUCCESS) {
            initStatus_ = "OpenXR: не удалось создать instance";
            return false;
        }
        return true;
    }

    bool GetSystem() {
        XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
        info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (xrGetSystem_(instance_, &info, &systemId_) != XR_SUCCESS) {
            initStatus_ = "OpenXR: HMD system не найден";
            return false;
        }
        return true;
    }

    bool CreateEglContext() {
        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay_ == EGL_NO_DISPLAY || !eglInitialize(eglDisplay_, nullptr, nullptr)) {
            initStatus_ = "EGL: display init failed";
            return false;
        }

        const EGLint cfg[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint count = 0;
        if (!eglChooseConfig(eglDisplay_, cfg, &eglConfig_, 1, &count) || count == 0) {
            initStatus_ = "EGL: config не найден";
            return false;
        }
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, ctxAttrs);
        if (eglContext_ == EGL_NO_CONTEXT) {
            initStatus_ = "EGL: context create failed";
            return false;
        }
        const EGLint surfAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, surfAttrs);
        if (eglSurface_ == EGL_NO_SURFACE || eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) != EGL_TRUE) {
            initStatus_ = "EGL: make current failed";
            return false;
        }
        return true;
    }

    bool CreateSession() {
        XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (xrGetOpenGLESGraphicsRequirementsKHR_(instance_, systemId_, &req) != XR_SUCCESS) {
            initStatus_ = "OpenXR: graphics requirements failed";
            return false;
        }

        XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = eglDisplay_;
        binding.config = eglConfig_;
        binding.context = eglContext_;

        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        info.systemId = systemId_;
        if (xrCreateSession_(instance_, &info, &session_) != XR_SUCCESS) {
            initStatus_ = "OpenXR: session create failed";
            return false;
        }
        return true;
    }

    bool CreateReferenceSpace() {
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        info.poseInReferenceSpace.orientation = {0.f, 0.f, 0.f, 1.f};
        info.poseInReferenceSpace.position = {0.f, 0.f, 0.f};
        if (xrCreateReferenceSpace_(session_, &info, &appSpace_) != XR_SUCCESS) {
            initStatus_ = "OpenXR: reference space failed";
            return false;
        }
        return true;
    }

    bool CreateSwapchains() {
        uint32_t count = 0;
        if (!CheckXr(
                xrEnumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
                "xrEnumerateViewConfigurationViews(count)")) {
            return false;
        }
        if (count < 2) {
            initStatus_ = "OpenXR: stereo view configuration недоступна";
            return false;
        }
        std::vector<XrViewConfigurationView> cfg(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        if (!CheckXr(
                xrEnumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, cfg.data()),
                "xrEnumerateViewConfigurationViews(data)")) {
            return false;
        }

        views_.resize(count, {XR_TYPE_VIEW});
        projectionViews_.resize(count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        const int64_t selectedFormat = ChooseSwapchainFormat();
        if (selectedFormat == 0) {
            return false;
        }

        for (uint32_t eye = 0; eye < 2; ++eye) {
            swapchains_[eye].width = static_cast<int32_t>(cfg[eye].recommendedImageRectWidth);
            swapchains_[eye].height = static_cast<int32_t>(cfg[eye].recommendedImageRectHeight);

            XrSwapchainCreateInfo scInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            scInfo.arraySize = 1;
            scInfo.mipCount = 1;
            scInfo.faceCount = 1;
            scInfo.sampleCount = cfg[eye].recommendedSwapchainSampleCount;
            scInfo.width = cfg[eye].recommendedImageRectWidth;
            scInfo.height = cfg[eye].recommendedImageRectHeight;
            scInfo.format = selectedFormat;
            scInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            if (!CheckXr(xrCreateSwapchain_(session_, &scInfo, &swapchains_[eye].handle), "xrCreateSwapchain")) {
                return false;
            }

            uint32_t imageCount = 0;
            if (!CheckXr(xrEnumerateSwapchainImages_(swapchains_[eye].handle, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages(count)")) {
                return false;
            }
            swapchains_[eye].images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            if (!CheckXr(
                    xrEnumerateSwapchainImages_(swapchains_[eye].handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchains_[eye].images.data())),
                    "xrEnumerateSwapchainImages(data)")) {
                return false;
            }
            swapchains_[eye].framebuffers.resize(imageCount, 0);
            glGenFramebuffers(static_cast<GLsizei>(imageCount), swapchains_[eye].framebuffers.data());
        }
        return true;
    }

    bool CreateProgramAndGeometry() {
        const char* vs = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUv;
uniform mat4 uTextureMatrix;
out vec2 vUv;
void main() {
    vec4 t = uTextureMatrix * vec4(aUv, 0.0, 1.0);
    vUv = t.xy;
    gl_Position = vec4(aPos, 0.0, 1.0);
})";
        const char* fs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTexture;
uniform float uRotation;
uniform vec2 uMirror;
in vec2 vUv;
out vec4 fragColor;
void main() {
    vec2 centered = vUv - vec2(0.5, 0.5);
    float s = sin(uRotation);
    float c = cos(uRotation);
    mat2 rot = mat2(c, -s, s, c);
    vec2 uv = (rot * (centered * uMirror)) + vec2(0.5, 0.5);
    fragColor = texture(uTexture, uv);
})";

        GLuint v = CompileShader(GL_VERTEX_SHADER, vs);
        GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) {
            initStatus_ = "GLES: shader compile failed";
            return false;
        }

        program_ = glCreateProgram();
        glAttachShader(program_, v);
        glAttachShader(program_, f);
        glLinkProgram(program_);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint linked = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) {
            initStatus_ = "GLES: shader link failed";
            return false;
        }

        const float quad[] = {
            -1.f, -1.f, 0.f, 0.f,
             1.f, -1.f, 1.f, 0.f,
            -1.f,  1.f, 0.f, 1.f,
             1.f,  1.f, 1.f, 1.f
        };
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

        uTextureLocation_ = glGetUniformLocation(program_, "uTexture");
        uTextureMatrixLocation_ = glGetUniformLocation(program_, "uTextureMatrix");
        uRotationLocation_ = glGetUniformLocation(program_, "uRotation");
        uMirrorLocation_ = glGetUniformLocation(program_, "uMirror");
        return true;
    }

    GLuint CompileShader(GLenum type, const char* src) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    void CreateCameraTextures() {
        glGenTextures(2, cameraTextureIds_.data());
        for (GLuint texId : cameraTextureIds_) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, texId);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    bool CreateActions() {
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(setInfo.actionSetName, "invertoscope_actions", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(setInfo.localizedActionSetName, "Invertoscope Actions", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (xrCreateActionSet_(instance_, &setInfo, &actionSet_) != XR_SUCCESS) {
            initStatus_ = "OpenXR: action set failed";
            return false;
        }

        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/left", &leftHandPath_), "xrStringToPath(left)")) return false;
        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/right", &rightHandPath_), "xrStringToPath(right)")) return false;
        std::array<XrPath, 2> hands{leftHandPath_, rightHandPath_};

        XrActionCreateInfo mirrorLeft{XR_TYPE_ACTION_CREATE_INFO};
        mirrorLeft.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        std::strncpy(mirrorLeft.actionName, "mirror_left", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(mirrorLeft.localizedActionName, "Mirror Left", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        mirrorLeft.countSubactionPaths = static_cast<uint32_t>(hands.size());
        mirrorLeft.subactionPaths = hands.data();
        if (!CheckXr(xrCreateAction_(actionSet_, &mirrorLeft, &mirrorLeftAction_), "xrCreateAction(mirror_left)")) return false;

        XrActionCreateInfo mirrorRight{XR_TYPE_ACTION_CREATE_INFO};
        mirrorRight.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        std::strncpy(mirrorRight.actionName, "mirror_right", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(mirrorRight.localizedActionName, "Mirror Right", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        mirrorRight.countSubactionPaths = static_cast<uint32_t>(hands.size());
        mirrorRight.subactionPaths = hands.data();
        if (!CheckXr(xrCreateAction_(actionSet_, &mirrorRight, &mirrorRightAction_), "xrCreateAction(mirror_right)")) return false;

        XrActionCreateInfo rotateLeft{XR_TYPE_ACTION_CREATE_INFO};
        rotateLeft.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        std::strncpy(rotateLeft.actionName, "rotate_left", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(rotateLeft.localizedActionName, "Rotate Left", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        rotateLeft.countSubactionPaths = static_cast<uint32_t>(hands.size());
        rotateLeft.subactionPaths = hands.data();
        if (!CheckXr(xrCreateAction_(actionSet_, &rotateLeft, &rotateLeftAction_), "xrCreateAction(rotate_left)")) return false;

        XrActionCreateInfo rotateRight{XR_TYPE_ACTION_CREATE_INFO};
        rotateRight.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        std::strncpy(rotateRight.actionName, "rotate_right", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(rotateRight.localizedActionName, "Rotate Right", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        rotateRight.countSubactionPaths = static_cast<uint32_t>(hands.size());
        rotateRight.subactionPaths = hands.data();
        if (!CheckXr(xrCreateAction_(actionSet_, &rotateRight, &rotateRightAction_), "xrCreateAction(rotate_right)")) return false;

        XrPath profile = XR_NULL_PATH;
        XrPath leftX = XR_NULL_PATH;
        XrPath rightA = XR_NULL_PATH;
        XrPath leftStickX = XR_NULL_PATH;
        XrPath rightStickX = XR_NULL_PATH;
        if (!CheckXr(xrStringToPath_(instance_, "/interaction_profiles/oculus/touch_controller", &profile), "xrStringToPath(profile)")) return false;
        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/left/input/x/click", &leftX), "xrStringToPath(leftX)")) return false;
        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/right/input/a/click", &rightA), "xrStringToPath(rightA)")) return false;
        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/left/input/thumbstick/x", &leftStickX), "xrStringToPath(leftStickX)")) return false;
        if (!CheckXr(xrStringToPath_(instance_, "/user/hand/right/input/thumbstick/x", &rightStickX), "xrStringToPath(rightStickX)")) return false;

        std::array<XrActionSuggestedBinding, 4> bindings{{
            {mirrorLeftAction_, leftX},
            {mirrorRightAction_, rightA},
            {rotateLeftAction_, leftStickX},
            {rotateRightAction_, rightStickX},
        }};

        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.suggestedBindings = bindings.data();
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        if (!CheckXr(xrSuggestInteractionProfileBindings_(instance_, &suggested), "xrSuggestInteractionProfileBindings", false)) {
            inputEnabled_ = false;
            LOGE("Input bindings disabled, rendering will continue without controller mapping.");
        }

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &actionSet_;
        if (!CheckXr(xrAttachSessionActionSets_(session_, &attach), "xrAttachSessionActionSets", false)) {
            inputEnabled_ = false;
            LOGE("Action sets attach failed, rendering will continue without input.");
        }
        return true;
    }

    bool GetActionBool(XrAction action, XrPath subPath) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subPath;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (xrGetActionStateBoolean_(session_, &info, &state) != XR_SUCCESS) return false;
        return state.isActive && state.currentState;
    }

    float GetActionFloat(XrAction action, XrPath subPath) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subPath;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (xrGetActionStateFloat_(session_, &info, &state) != XR_SUCCESS) return 0.f;
        return state.isActive ? state.currentState : 0.f;
    }

    void SyncInput(XrDuration predictedDisplayPeriod) {
        if (!inputEnabled_) return;
        XrActiveActionSet active{};
        active.actionSet = actionSet_;
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (!CheckXr(xrSyncActions_(session_, &sync), "xrSyncActions", false)) {
            inputEnabled_ = false;
            LOGE("xrSyncActions failed, input disabled for current session.");
            return;
        }

        const bool leftPressed = GetActionBool(mirrorLeftAction_, leftHandPath_);
        const bool rightPressed = GetActionBool(mirrorRightAction_, rightHandPath_);
        const float leftAxis = GetActionFloat(rotateLeftAction_, leftHandPath_);
        const float rightAxis = GetActionFloat(rotateRightAction_, rightHandPath_);
        const float dt = static_cast<float>(predictedDisplayPeriod) / 1'000'000'000.0f;

        std::lock_guard<std::mutex> lock(dataMutex_);
        if (leftPressed && !prevLeftPressed_) transforms_[0].mirrorX = !transforms_[0].mirrorX;
        if (rightPressed && !prevRightPressed_) transforms_[1].mirrorX = !transforms_[1].mirrorX;
        transforms_[0].rotationDegrees = WrapDeg(transforms_[0].rotationDegrees + leftAxis * 120.f * dt);
        transforms_[1].rotationDegrees = WrapDeg(transforms_[1].rotationDegrees + rightAxis * 120.f * dt);
        prevLeftPressed_ = leftPressed;
        prevRightPressed_ = rightPressed;
    }

    float WrapDeg(float v) {
        while (v > 360.f) v -= 360.f;
        while (v < 0.f) v += 360.f;
        return v;
    }

    void UpdateCameraTextures(JNIEnv* env) {
        if (!callback_ || !callbackMethod_) return;
        env->CallBooleanMethod(callback_, callbackMethod_);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    void RenderStereoViews(uint32_t viewCount) {
        glUseProgram(program_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(0));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(sizeof(float) * 2));
        for (uint32_t eye = 0; eye < viewCount && eye < 2; ++eye) {
            RenderEye(eye);
        }
    }

    void RenderEye(uint32_t eye) {
        auto& sc = swapchains_[eye];
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!CheckXr(xrAcquireSwapchainImage_(sc.handle, &acquire, &imageIndex), "xrAcquireSwapchainImage", false)) return;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!CheckXr(xrWaitSwapchainImage_(sc.handle, &wait), "xrWaitSwapchainImage", false)) return;

        glBindFramebuffer(GL_FRAMEBUFFER, sc.framebuffers[imageIndex]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[imageIndex].image, 0);
        glViewport(0, 0, sc.width, sc.height);
        glClearColor(0.02f, 0.02f, 0.02f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::array<float, 16> mat{};
        EyeTransform tr{};
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            mat = textureMatrices_[eye];
            tr = transforms_[eye];
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, cameraTextureIds_[eye]);
        glUniform1i(uTextureLocation_, 0);
        glUniformMatrix4fv(uTextureMatrixLocation_, 1, GL_FALSE, mat.data());
        glUniform1f(uRotationLocation_, tr.rotationDegrees * (3.14159265f / 180.f));
        glUniform2f(uMirrorLocation_, tr.mirrorX ? -1.f : 1.f, tr.mirrorY ? -1.f : 1.f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CheckXr(xrReleaseSwapchainImage_(sc.handle, &release), "xrReleaseSwapchainImage", false);

        projectionViews_[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        projectionViews_[eye].pose = views_[eye].pose;
        projectionViews_[eye].fov = views_[eye].fov;
        projectionViews_[eye].subImage.swapchain = sc.handle;
        projectionViews_[eye].subImage.imageArrayIndex = 0;
        projectionViews_[eye].subImage.imageRect.offset = {0, 0};
        projectionViews_[eye].subImage.imageRect.extent = {sc.width, sc.height};
    }

    void EndFrameProjection(XrTime displayTime, uint32_t viewCount) {
        projectionLayer_ = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projectionLayer_.space = appSpace_;
        projectionLayer_.viewCount = viewCount;
        projectionLayer_.views = projectionViews_.data();
        const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer_);

        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = displayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 1;
        end.layers = &layer;
        xrEndFrame_(session_, &end);
    }

    void EndFrameNoLayers(XrTime displayTime) {
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = displayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 0;
        end.layers = nullptr;
        xrEndFrame_(session_, &end);
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent_(instance_, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                sessionState_ = changed->state;
                if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (xrBeginSession_(session_, &begin) == XR_SUCCESS) sessionRunning_ = true;
                } else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                    xrEndSession_(session_);
                    sessionRunning_ = false;
                } else if (sessionState_ == XR_SESSION_STATE_EXITING || sessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
                    running_ = false;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void InitIdentityMatrices() {
        for (auto& m : textureMatrices_) {
            m = {1.f, 0.f, 0.f, 0.f,
                 0.f, 1.f, 0.f, 0.f,
                 0.f, 0.f, 1.f, 0.f,
                 0.f, 0.f, 0.f, 1.f};
        }
    }

    void ShutdownRuntime() {
        for (auto& sc : swapchains_) {
            if (!sc.framebuffers.empty()) {
                glDeleteFramebuffers(static_cast<GLsizei>(sc.framebuffers.size()), sc.framebuffers.data());
            }
            if (sc.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain_(sc.handle);
            }
        }
        if (cameraTextureIds_[0] != 0 || cameraTextureIds_[1] != 0) {
            glDeleteTextures(2, cameraTextureIds_.data());
            cameraTextureIds_ = {0, 0};
        }
        if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
        if (program_ != 0) glDeleteProgram(program_);

        if (sessionRunning_) xrEndSession_(session_);
        if (actionSet_ != XR_NULL_HANDLE) {
            if (mirrorLeftAction_ != XR_NULL_HANDLE) xrDestroyAction_(mirrorLeftAction_);
            if (mirrorRightAction_ != XR_NULL_HANDLE) xrDestroyAction_(mirrorRightAction_);
            if (rotateLeftAction_ != XR_NULL_HANDLE) xrDestroyAction_(rotateLeftAction_);
            if (rotateRightAction_ != XR_NULL_HANDLE) xrDestroyAction_(rotateRightAction_);
            xrDestroyActionSet_(actionSet_);
        }
        if (appSpace_ != XR_NULL_HANDLE) xrDestroySpace_(appSpace_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession_(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance_(instance_);

        if (eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
            if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
            eglTerminate(eglDisplay_);
        }
        if (loaderHandle_) dlclose(loaderHandle_);
    }

private:
    JavaVM* javaVm_ = nullptr;
    jobject activity_ = nullptr;
    jobject callback_ = nullptr;
    jmethodID callbackMethod_ = nullptr;

    std::thread renderThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool initDone_ = false;
    std::string initStatus_;
    std::mutex initMutex_;
    std::condition_variable initCv_;

    std::mutex dataMutex_;
    std::array<std::array<float, 16>, 2> textureMatrices_{};
    std::array<EyeTransform, 2> transforms_{};
    std::array<GLuint, 2> cameraTextureIds_{0, 0};
    bool prevLeftPressed_ = false;
    bool prevRightPressed_ = false;
    bool inputEnabled_ = true;

    void* loaderHandle_ = nullptr;

    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;
    PFN_xrCreateInstance xrCreateInstance_ = nullptr;
    PFN_xrDestroyInstance xrDestroyInstance_ = nullptr;
    PFN_xrGetSystem xrGetSystem_ = nullptr;
    PFN_xrCreateSession xrCreateSession_ = nullptr;
    PFN_xrDestroySession xrDestroySession_ = nullptr;
    PFN_xrCreateReferenceSpace xrCreateReferenceSpace_ = nullptr;
    PFN_xrDestroySpace xrDestroySpace_ = nullptr;
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews_ = nullptr;
    PFN_xrCreateSwapchain xrCreateSwapchain_ = nullptr;
    PFN_xrDestroySwapchain xrDestroySwapchain_ = nullptr;
    PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats_ = nullptr;
    PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages_ = nullptr;
    PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage_ = nullptr;
    PFN_xrWaitSwapchainImage xrWaitSwapchainImage_ = nullptr;
    PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage_ = nullptr;
    PFN_xrWaitFrame xrWaitFrame_ = nullptr;
    PFN_xrBeginFrame xrBeginFrame_ = nullptr;
    PFN_xrEndFrame xrEndFrame_ = nullptr;
    PFN_xrLocateViews xrLocateViews_ = nullptr;
    PFN_xrPollEvent xrPollEvent_ = nullptr;
    PFN_xrBeginSession xrBeginSession_ = nullptr;
    PFN_xrEndSession xrEndSession_ = nullptr;
    PFN_xrStringToPath xrStringToPath_ = nullptr;
    PFN_xrCreateActionSet xrCreateActionSet_ = nullptr;
    PFN_xrDestroyActionSet xrDestroyActionSet_ = nullptr;
    PFN_xrCreateAction xrCreateAction_ = nullptr;
    PFN_xrDestroyAction xrDestroyAction_ = nullptr;
    PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings_ = nullptr;
    PFN_xrAttachSessionActionSets xrAttachSessionActionSets_ = nullptr;
    PFN_xrSyncActions xrSyncActions_ = nullptr;
    PFN_xrGetActionStateBoolean xrGetActionStateBoolean_ = nullptr;
    PFN_xrGetActionStateFloat xrGetActionStateFloat_ = nullptr;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR_ = nullptr;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    std::array<SwapchainBundle, 2> swapchains_{};
    std::vector<XrView> views_;
    std::vector<XrCompositionLayerProjectionView> projectionViews_;
    XrCompositionLayerProjection projectionLayer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLint uTextureLocation_ = -1;
    GLint uTextureMatrixLocation_ = -1;
    GLint uRotationLocation_ = -1;
    GLint uMirrorLocation_ = -1;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction mirrorLeftAction_ = XR_NULL_HANDLE;
    XrAction mirrorRightAction_ = XR_NULL_HANDLE;
    XrAction rotateLeftAction_ = XR_NULL_HANDLE;
    XrAction rotateRightAction_ = XR_NULL_HANDLE;
    XrPath leftHandPath_ = XR_NULL_PATH;
    XrPath rightHandPath_ = XR_NULL_PATH;
};

InvertoscopeRuntime gRuntime;

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_start(JNIEnv* env, jobject /*thiz*/, jobject activity, jobject callback) {
    std::string status = gRuntime.Start(env, activity, callback);
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_stop(JNIEnv* env, jobject /*thiz*/) {
    gRuntime.Stop(env);
}

extern "C" JNIEXPORT void JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_onResume(JNIEnv* /*env*/, jobject /*thiz*/) {
    gRuntime.OnResume();
}

extern "C" JNIEXPORT void JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_onPause(JNIEnv* /*env*/, jobject /*thiz*/) {
    gRuntime.OnPause();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_getCameraTextureId(JNIEnv* /*env*/, jobject /*thiz*/, jint eye) {
    return gRuntime.GetCameraTextureId(static_cast<int>(eye));
}

extern "C" JNIEXPORT void JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_setCameraTextureMatrix(JNIEnv* env, jobject /*thiz*/, jint eye, jfloatArray matrix4x4) {
    if (!matrix4x4) return;
    if (env->GetArrayLength(matrix4x4) < 16) return;
    jfloat values[16];
    env->GetFloatArrayRegion(matrix4x4, 0, 16, values);
    gRuntime.SetCameraTextureMatrix(static_cast<int>(eye), values);
}
