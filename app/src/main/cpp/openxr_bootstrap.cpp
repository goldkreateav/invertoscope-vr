#include <jni.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <string>

using PFN_xrCreateInstance = int (*)(const void* createInfo, void* instance);
using PFN_xrDestroyInstance = int (*)(void* instance);
using PFN_xrGetSystem = int (*)(void* instance, const void* getInfo, unsigned long long* systemId);

namespace {

constexpr int XR_SUCCESS = 0;
constexpr int XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1;
constexpr int XR_TYPE_INSTANCE_CREATE_INFO = 3;
constexpr int XR_TYPE_SYSTEM_GET_INFO = 4;

struct XrApplicationInfo {
    char applicationName[128];
    uint32_t applicationVersion;
    char engineName[128];
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct XrInstanceCreateInfo {
    int type;
    const void* next;
    uint32_t createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
};

struct XrSystemGetInfo {
    int type;
    const void* next;
    int formFactor;
};

std::string BootstrapOpenXr() {
    void* loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        return "OpenXR: загрузчик не найден";
    }

    auto xrCreateInstance =
        reinterpret_cast<PFN_xrCreateInstance>(dlsym(loader, "xrCreateInstance"));
    auto xrDestroyInstance =
        reinterpret_cast<PFN_xrDestroyInstance>(dlsym(loader, "xrDestroyInstance"));
    auto xrGetSystem =
        reinterpret_cast<PFN_xrGetSystem>(dlsym(loader, "xrGetSystem"));

    if (!xrCreateInstance || !xrDestroyInstance || !xrGetSystem) {
        dlclose(loader);
        return "OpenXR: символы загрузчика не найдены";
    }

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::snprintf(createInfo.applicationInfo.applicationName,
                  sizeof(createInfo.applicationInfo.applicationName),
                  "QuestInvertoscope");
    std::snprintf(createInfo.applicationInfo.engineName,
                  sizeof(createInfo.applicationInfo.engineName),
                  "NativeOpenGLES");
    createInfo.applicationInfo.apiVersion = 1;

    void* instance = nullptr;
    int createResult = xrCreateInstance(&createInfo, &instance);
    if (createResult != XR_SUCCESS || instance == nullptr) {
        dlclose(loader);
        return "OpenXR: среда выполнения недоступна";
    }

    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    unsigned long long systemId = 0;
    int systemResult = xrGetSystem(instance, &systemInfo, &systemId);
    xrDestroyInstance(instance);
    dlclose(loader);

    if (systemResult != XR_SUCCESS || systemId == 0) {
        return "OpenXR: шлем не найден";
    }

    return "OpenXR: загрузчик ОК, шлем обнаружен";
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_com_invertoscope_quest_xr_OpenXrBridge_bootstrapRuntimeInfo(JNIEnv* env, jobject /* this */) {
    std::string message = BootstrapOpenXr();
    return env->NewStringUTF(message.c_str());
}
