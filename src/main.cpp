#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <stdio.h>

#include <dobby.h>

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

// --- 전역 변수 ---
static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static bool show_menu = false;
static bool my_checkbox_state = false;
static bool last_checkbox_state = false; 

// --- 마인크래프트 오프셋 관련 변수 ---
uintptr_t mcpe_base = 0;
void* g_localPlayer = nullptr;

void (*displayClientMessage)(void* _this, const std::string& msg) = nullptr;
void (*orig_normalTick)(void* _this) = nullptr;

uintptr_t get_lib_base(const char* lib_name) {
    FILE* fp = fopen("/proc/self/maps", "rt");
    if (!fp) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, lib_name)) {
            if (strstr(line, "r-xp") || strstr(line, "r--p")) {
                sscanf(line, "%lx-", &base);
                break;
            }
        }
    }
    fclose(fp);
    return base;
}

void hook_normalTick(void* _this) {
    g_localPlayer = _this; 
    if (orig_normalTick) {
        orig_normalTick(_this); 
    }
}

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;
static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

static void hookinput() {
    void* hInput = dlopen("libinput.so", RTLD_LAZY);
    if (hInput) {
        void* sym = dlsym(hInput, "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE");
        if (sym) {
            DobbyHook(sym, (void*)hook_input2, (void**)&orig_input2);
        }
    }
}

static void draw_toggle_button() {
    ImGui::SetNextWindowPos(ImVec2(50.0f, 50.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##ToggleBtn", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::SetWindowFontScale(2.0f);
    if (ImGui::Button(show_menu ? "Close" : "Menu", ImVec2(150, 80))) {
        show_menu = !show_menu;
    }
    ImGui::End();
}

static void draw_menu() {
    if (!show_menu) return;

    ImGui::SetNextWindowPos(ImVec2(g_width * 0.5f, g_height * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Araxis Client", &show_menu, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(2.0f);

    ImGui::Checkbox("Test Module", &my_checkbox_state);
    
    if (my_checkbox_state != last_checkbox_state) {
        last_checkbox_state = my_checkbox_state; 

        if (g_localPlayer != nullptr && displayClientMessage != nullptr) {
            std::string msg;
            if (my_checkbox_state) {
                msg = "\xC2\xA7\x61[Araxis] Test Module ON!"; // 초록색 텍스트
            } else {
                msg = "\xC2\xA7\x63[Araxis] Test Module OFF!"; // 빨간색 텍스트
            }
            
            displayClientMessage(g_localPlayer, msg);
        }
    }

    ImGui::Separator();

    if (my_checkbox_state) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ON!");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "OFF!");
    }

    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::StyleColorsDark();
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    GLint last_prog, last_tex, last_abuf, last_ebuf, last_fbo, last_vp[4];
    GLboolean last_blend, last_depth, last_scissor;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_abuf); glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_ebuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo); glGetIntegerv(GL_VIEWPORT, last_vp);
    last_blend = glIsEnabled(GL_BLEND); last_depth = glIsEnabled(GL_DEPTH_TEST); last_scissor = glIsEnabled(GL_SCISSOR_TEST);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    
    draw_toggle_button();
    draw_menu();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glUseProgram(last_prog); glBindTexture(GL_TEXTURE_2D, last_tex);
    glBindBuffer(GL_ARRAY_BUFFER, last_abuf); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_ebuf);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo); glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);
    
    if (g_targetcontext == EGL_NO_CONTEXT) { 
        g_targetcontext = ctx; 
        g_targetsurface = surf; 
    }
    if (ctx != g_targetcontext || surf != g_targetsurface) return orig_eglswapbuffers(dpy, surf);
    
    g_width = w; 
    g_height = h;
    
    setup();
    render();
    
    return orig_eglswapbuffers(dpy, surf);
}

static void* mainthread(void*) {
    // 마인크래프트 라이브러리가 완전히 로드될 때까지 대기
    while (mcpe_base == 0) {
        mcpe_base = get_lib_base("libminecraftpe.so");
        if (mcpe_base == 0) sleep(1);
    }
    
    // 1. 함수 바인딩
    displayClientMessage = (void(*)(void*, const std::string&))(mcpe_base + 0xA6B2C60);

    // 2. DobbyHook 실행
    void* tick_target = (void*)(mcpe_base + 0xEC4E028);
    DobbyHook(tick_target, (void*)hook_normalTick, (void**)&orig_normalTick);

    void* hEGL = dlopen("libEGL.so", RTLD_LAZY);
    if (hEGL) {
        void* swap = dlsym(hEGL, "eglSwapBuffers");
        if (swap) {
            DobbyHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
        }
    }

    hookinput(); 
    
    return nullptr;
}

__attribute__((constructor))
void display_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
