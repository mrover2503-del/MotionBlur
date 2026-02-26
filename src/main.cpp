#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <mutex>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

const char* vertexShaderSource = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

const char* blendFragmentShaderSource = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uCurrentFrame;
uniform sampler2D uHistoryFrame;
uniform float uBlendFactor;
void main() {
    vec4 current = texture2D(uCurrentFrame, vTexCoord);
    vec4 history = texture2D(uHistoryFrame, vTexCoord);
    vec4 result = mix(current, history, uBlendFactor);
    gl_FragColor = vec4(result.rgb, 1.0);
}
)";

const char* drawFragmentShaderSource = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    gl_FragColor = vec4(color.rgb, 1.0);
}
)";

static bool motion_blur_enabled = false;
static float blur_strength = 0.85f;

static GLuint rawTexture = 0;
static GLuint historyTextures[2] = {0, 0};
static GLuint historyFBOs[2] = {0, 0};
static int pingPongIndex = 0;
static bool isFirstFrame = true;

static GLuint blendShaderProgram = 0;
static GLint blendPosLoc = -1;
static GLint blendTexCoordLoc = -1;
static GLint blendCurrentLoc = -1;
static GLint blendHistoryLoc = -1;
static GLint blendFactorLoc = -1;

static GLuint drawShaderProgram = 0;
static GLint drawPosLoc = -1;
static GLint drawTexCoordLoc = -1;
static GLint drawTextureLoc = -1;

static GLuint vertexBuffer = 0;
static GLuint indexBuffer = 0;

static int blur_res_width = 0;
static int blur_res_height = 0;

void initializeMotionBlurResources(GLint width, GLint height) {
    if (rawTexture != 0) {
        glDeleteTextures(1, &rawTexture);
        glDeleteTextures(2, historyTextures);
        glDeleteFramebuffers(2, historyFBOs);
    }

    if (blendShaderProgram == 0) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vertexShaderSource, nullptr);
        glCompileShader(vs);

        GLuint fsBlend = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsBlend, 1, &blendFragmentShaderSource, nullptr);
        glCompileShader(fsBlend);

        blendShaderProgram = glCreateProgram();
        glAttachShader(blendShaderProgram, vs);
        glAttachShader(blendShaderProgram, fsBlend);
        glLinkProgram(blendShaderProgram);

        blendPosLoc = glGetAttribLocation(blendShaderProgram, "aPosition");
        blendTexCoordLoc = glGetAttribLocation(blendShaderProgram, "aTexCoord");
        blendCurrentLoc = glGetUniformLocation(blendShaderProgram, "uCurrentFrame");
        blendHistoryLoc = glGetUniformLocation(blendShaderProgram, "uHistoryFrame");
        blendFactorLoc = glGetUniformLocation(blendShaderProgram, "uBlendFactor");

        GLuint fsDraw = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsDraw, 1, &drawFragmentShaderSource, nullptr);
        glCompileShader(fsDraw);

        drawShaderProgram = glCreateProgram();
        glAttachShader(drawShaderProgram, vs);
        glAttachShader(drawShaderProgram, fsDraw);
        glLinkProgram(drawShaderProgram);

        drawPosLoc = glGetAttribLocation(drawShaderProgram, "aPosition");
        drawTexCoordLoc = glGetAttribLocation(drawShaderProgram, "aTexCoord");
        drawTextureLoc = glGetUniformLocation(drawShaderProgram, "uTexture");

        GLfloat vertices[] = { 
            -1.0f, 1.0f, 0.0f, 1.0f, 
            -1.0f, -1.0f, 0.0f, 0.0f, 
            1.0f, -1.0f, 1.0f, 0.0f, 
            1.0f, 1.0f, 1.0f, 1.0f 
        };
        GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

        glGenBuffers(1, &vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glGenBuffers(1, &indexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    }

    glGenTextures(1, &rawTexture);
    glBindTexture(GL_TEXTURE_2D, rawTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(2, historyTextures);
    glGenFramebuffers(2, historyFBOs);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, historyTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, historyTextures[i], 0);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    blur_res_width = width;
    blur_res_height = height;
    pingPongIndex = 0;
    isFirstFrame = true;
}

void apply_motion_blur(int width, int height) {
    if (width != blur_res_width || height != blur_res_height || rawTexture == 0) {
        initializeMotionBlurResources(width, height);
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glBindTexture(GL_TEXTURE_2D, rawTexture);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);

    int curr = pingPongIndex;
    int prev = 1 - pingPongIndex;

    if (isFirstFrame) {
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[curr]);
        glViewport(0, 0, width, height);
        
        glUseProgram(drawShaderProgram);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, rawTexture);
        glUniform1i(drawTextureLoc, 0);
        
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        
        glEnableVertexAttribArray(drawPosLoc);
        glVertexAttribPointer(drawPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(drawTexCoordLoc);
        glVertexAttribPointer(drawTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        
        isFirstFrame = false;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[curr]);
        glViewport(0, 0, width, height);
        
        glUseProgram(blendShaderProgram);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, rawTexture);
        glUniform1i(blendCurrentLoc, 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, historyTextures[prev]);
        glUniform1i(blendHistoryLoc, 1);

        glUniform1f(blendFactorLoc, blur_strength);

        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);

        glEnableVertexAttribArray(blendPosLoc);
        glVertexAttribPointer(blendPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(blendTexCoordLoc);
        glVertexAttribPointer(blendTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(drawShaderProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, historyTextures[curr]);
    glUniform1i(drawTextureLoc, 0);

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);

    glEnableVertexAttribArray(drawPosLoc);
    glVertexAttribPointer(drawPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(drawTexCoordLoc);
    glVertexAttribPointer(drawTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    pingPongIndex = prev;
}

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

struct WindowBounds {
    float x, y, w, h;
    bool visible;
};
static WindowBounds g_menuBounds = {0, 0, 0, 0, false};
static std::mutex g_boundsMutex;



void DrawMenu() {
    static bool show_menu = false;
    static bool motion_blur_enabled = true;
    static int current_tab = 0;

    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(20.0f, io.DisplaySize.y - 60.0f), ImGuiCond_Always);
    ImGui::Begin("MenuTrigger", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button("OPEN MENU", ImVec2(100, 40))) {
        show_menu = !show_menu;
    }
    ImGui::PopStyleColor(2);
    ImGui::End();

    if (show_menu) {
        ImGui::SetNextWindowSize(ImVec2(550, 350), ImGuiCond_FirstUseEver);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.0f));

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("CustomUI_Main", &show_menu, window_flags);

        ImVec2 win_pos = ImGui::GetWindowPos();
        ImVec2 win_size = ImGui::GetWindowSize();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        float time = (float)ImGui::GetTime();
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(fmodf(time * 0.5f, 1.0f), 1.0f, 1.0f, r, g, b);
        
        draw_list->AddRectFilled(win_pos, ImVec2(win_pos.x + win_size.x, win_pos.y + 3.0f), ImColor(r, g, b));

        ImGui::SetCursorPosY(3.0f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.04f, 1.0f));
        ImGui::BeginChild("Sidebar", ImVec2(60, win_size.y - 3.0f), false);

        for (int i = 0; i < 4; ++i) {
            ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
            ImVec2 center = ImVec2(cursor_pos.x + 30.0f, cursor_pos.y + 30.0f);
            
            if (current_tab == i) {
                draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + 60.0f, cursor_pos.y + 60.0f), ImColor(0.12f, 0.12f, 0.12f, 1.0f));
            }

            if (ImGui::InvisibleButton((std::string("tab_") + std::to_string(i)).c_str(), ImVec2(60.0f, 60.0f))) {
                current_tab = i;
            }

            ImU32 icon_color = (current_tab == i) ? ImColor(255, 255, 255) : ImColor(150, 150, 150);
            
            if (i == 0) {
                draw_list->AddCircleFilled(ImVec2(center.x, center.y - 4), 6.0f, icon_color);
                draw_list->AddPathArcTo(center, 12.0f, 0.0f, 3.14159f);
                draw_list->PathStroke(icon_color, false, 3.0f);
            } else if (i == 1) {
                draw_list->AddRectFilled(ImVec2(center.x - 6, center.y - 6), ImVec2(center.x + 6, center.y + 6), icon_color);
                draw_list->AddRect(ImVec2(center.x - 10, center.y - 10), ImVec2(center.x + 10, center.y + 10), icon_color, 0.0f, 0, 2.0f);
            } else if (i == 2) {
                draw_list->AddLine(ImVec2(center.x - 10, center.y - 10), ImVec2(center.x + 10, center.y + 10), icon_color, 3.0f);
                draw_list->AddLine(ImVec2(center.x + 10, center.y - 10), ImVec2(center.x - 10, center.y + 10), icon_color, 3.0f);
            } else if (i == 3) {
                draw_list->AddLine(ImVec2(center.x - 10, center.y - 5), ImVec2(center.x + 10, center.y - 5), icon_color, 2.0f);
                draw_list->AddCircleFilled(ImVec2(center.x - 2, center.y - 5), 3.0f, icon_color);
                draw_list->AddLine(ImVec2(center.x - 10, center.y + 5), ImVec2(center.x + 10, center.y + 5), icon_color, 2.0f);
                draw_list->AddCircleFilled(ImVec2(center.x + 4, center.y + 5), 3.0f, icon_color);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 0);

        ImGui::BeginChild("Content", ImVec2(win_size.x - 60.0f, win_size.y - 3.0f), false);
        ImGui::SetCursorPos(ImVec2(20.0f, 20.0f));
        
        if (current_tab == 0) {
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            
            ImGui::Checkbox("MOTION BLUR ON/OFF", &motion_blur_enabled);
            
            ImGui::PopStyleColor(3);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Content for Tab %d", current_tab);
        }

        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}


static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    GLint last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    glActiveTexture(GL_TEXTURE0);
    GLint last_tex0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex0);
    glActiveTexture(GL_TEXTURE1);
    GLint last_tex1; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex1);
    glActiveTexture(last_active_texture);

    GLint last_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    GLint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend = glIsEnabled(GL_BLEND);

    if (motion_blur_enabled) {
        apply_motion_blur(g_width, g_height);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    
    drawmenu();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glUseProgram(last_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, last_tex0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, last_tex1);
    glActiveTexture(last_active_texture);

    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    if (blendPosLoc >= 0) glDisableVertexAttribArray(blendPosLoc);
    if (blendTexCoordLoc >= 0) glDisableVertexAttribArray(blendTexCoordLoc);
    if (drawPosLoc >= 0) glDisableVertexAttribArray(drawPosLoc);
    if (drawTexCoordLoc >= 0) glDisableVertexAttribArray(drawTexCoordLoc);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || (g_targetcontext != EGL_NO_CONTEXT && (ctx != g_targetcontext || surf != g_targetsurface)))
        return orig_eglswapbuffers(dpy, surf);
    
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    g_width = w; g_height = h;
    
    setup();
    render();
    
    return orig_eglswapbuffers(dpy, surf);
}

typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);

struct PreloaderInput_Interface {
    void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn callback);
};

typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();

bool OnTouchCallback(int action, int pointerId, float x, float y) {
    if (!g_initialized) return false;
    
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    
    if (action == AMOTION_EVENT_ACTION_DOWN) {
        io.AddMouseButtonEvent(0, true);
    } else if (action == AMOTION_EVENT_ACTION_UP) {
        io.AddMouseButtonEvent(0, false);
    }
    
    bool hitTest = false;
    {
        std::lock_guard<std::mutex> lock(g_boundsMutex);
        if (g_menuBounds.visible) {
            if (x >= g_menuBounds.x && x <= (g_menuBounds.x + g_menuBounds.w) &&
                y >= g_menuBounds.y && y <= (g_menuBounds.y + g_menuBounds.h)) {
                hitTest = true;
            }
        }
    }
    
    return hitTest || io.WantCaptureMouse;
}

static void* mainthread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");

    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    }

    void* preloaderLib = dlopen("libpreloader.so", RTLD_NOW);
    if (preloaderLib) {
        GetPreloaderInput_Fn GetInput = (GetPreloaderInput_Fn)dlsym(preloaderLib, "GetPreloaderInput");
        if (GetInput) {
            PreloaderInput_Interface* input = GetInput();
            if (input && input->RegisterTouchCallback) {
                input->RegisterTouchCallback(OnTouchCallback);
            }
        }
    }

    return nullptr;
}

__attribute__((constructor))
void display_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
