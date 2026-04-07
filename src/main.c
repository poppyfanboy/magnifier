#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <magnification.h>

#include <GL/gl.h>
#include <glcorearb.h>
#include <wglext.h>

#define MAGNIFIER_SIZE 256

#ifndef SCREEN_RECORDING_METHOD
    #define SCREEN_RECORDING_METHOD SCREEN_RECORDING_VIA_MAGNIFICATION_API
#endif

#define SRC(...) #__VA_ARGS__

typedef uint8_t u8;
typedef uint32_t u32;

static inline int int_clamp(int value, int min, int max) {
    if (value < min) {
        return min;
    } else if (value > max) {
        return max;
    } else {
        return value;
    }
}

typedef enum {
    // Faster, but excluded windows will be hidden in screen recording (e.g. with OBS).
    SCREEN_RECORDING_VIA_DEVICE_CONTEXT,

    // Slower, but excluded windows are still visible in screen recording.
    SCREEN_RECORDING_VIA_MAGNIFICATION_API,
} ScreenRecordingMethod;

typedef struct {
    ScreenRecordingMethod method;

    int screen_width;
    int screen_height;
    int screen_offset_x;
    int screen_offset_y;

    u32 *image;
    int image_width;
    int image_height;

    // Fields specific to the "Magnification API" method:
    HWND magnifier_window;
} ScreenRecorder;

BOOL CALLBACK magnification_api_scaling_callback(
    HWND window,
    void *source_data,
    MAGIMAGEHEADER source_header,
    void *destination_data,
    MAGIMAGEHEADER destination_header,
    RECT unclipped,
    RECT clipped,
    HRGN dirty
) {
    HWND magnifier_window = GetAncestor(window, GA_PARENT);
    ScreenRecorder *recorder = (ScreenRecorder *)GetWindowLongPtr(magnifier_window, GWLP_USERDATA);

    if (IsEqualGUID(&source_header.format, &GUID_WICPixelFormat32bppRGBA)) {
        int screen_from_x = int_clamp(recorder->screen_offset_x, 0, source_header.width);
        int screen_to_x = int_clamp(
            recorder->screen_offset_x + recorder->image_width,
            0, source_header.width
        );

        int image_x = screen_from_x - recorder->screen_offset_x;
        int image_clipped_width = screen_to_x - screen_from_x;

        int screen_from_y = int_clamp(recorder->screen_offset_y, 0, source_header.height);
        int screen_to_y = int_clamp(
            recorder->screen_offset_y + recorder->image_height,
            0, source_header.height
        );

        for (int screen_y = screen_from_y; screen_y < screen_to_y; screen_y += 1) {
            int image_y = screen_y - recorder->screen_offset_y;
            memcpy(
                &recorder->image[image_y * recorder->image_width + image_x],
                &((u8 *)source_data)[screen_y * source_header.stride + screen_from_x * sizeof(u32)],
                image_clipped_width * sizeof(u32)
            );
        }
    }

    return TRUE;
}

bool screen_recorder_create(
    ScreenRecordingMethod method,
    u32 *image,
    int image_width,
    int image_height,
    ScreenRecorder *recorder
) {
    recorder->method = method;

    recorder->screen_width = GetSystemMetrics(SM_CXSCREEN);
    recorder->screen_height = GetSystemMetrics(SM_CYSCREEN);
    recorder->screen_offset_x = 0;
    recorder->screen_offset_y = 0;

    recorder->image = image;
    recorder->image_width = image_width;
    recorder->image_height = image_height;

    if (recorder->method == SCREEN_RECORDING_VIA_DEVICE_CONTEXT) {
        return true;
    }

    if (recorder->method == SCREEN_RECORDING_VIA_MAGNIFICATION_API) {
        if (!MagInitialize()) {
            return false;
        }

        HINSTANCE instance = GetModuleHandle(NULL);

        WNDCLASS host_window_class = {
            .style = CS_HREDRAW | CS_VREDRAW,
            .lpfnWndProc = DefWindowProc,
            .hInstance = instance,
            .lpszClassName = L"Magnifier::MagnificationAPIHostWindow",
        };
        RegisterClass(&host_window_class);

        DWORD host_window_style = WS_SYSMENU | WS_CLIPCHILDREN | WS_CAPTION;

        RECT host_window_rect = {0, 0, recorder->screen_width, recorder->screen_height};
        AdjustWindowRect(&host_window_rect, host_window_style, FALSE);

        HWND host_window = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            host_window_class.lpszClassName,
            L"Magnification API Host Window",
            host_window_style,
            host_window_rect.left,
            host_window_rect.top,
            host_window_rect.right - host_window_rect.left,
            host_window_rect.bottom - host_window_rect.top,
            NULL, NULL, host_window_class.hInstance, NULL
        );

        recorder->magnifier_window = CreateWindowEx(
            0,
            WC_MAGNIFIER,
            L"Magnification API Magnifier Child Window",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            recorder->screen_width,
            recorder->screen_height,
            host_window, NULL, instance, NULL
        );

        SetWindowLongPtr(recorder->magnifier_window, GWLP_USERDATA, (LONG_PTR)recorder);
        MagSetImageScalingCallback(recorder->magnifier_window, magnification_api_scaling_callback);

        return true;
    }

    return false;
}

void screen_recorder_exclude_window(ScreenRecorder *recorder, HWND window) {
    switch (recorder->method) {
        case SCREEN_RECORDING_VIA_DEVICE_CONTEXT: {
            SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
        } break;

        case SCREEN_RECORDING_VIA_MAGNIFICATION_API: {
            MagSetWindowFilterList(
                recorder->magnifier_window,
                MW_FILTERMODE_EXCLUDE,
                1,
                &window
            );
        } break;
    }
}

void screen_recorder_snap_via_device_context(ScreenRecorder *recorder, int x, int y) {
    HDC screen_device_context = GetDC(NULL);

    HDC capture_device_context = CreateCompatibleDC(screen_device_context);
    HBITMAP capture_bitmap = CreateCompatibleBitmap(
        screen_device_context,
        MAGNIFIER_SIZE,
        MAGNIFIER_SIZE
    );
    HBITMAP capture_bitmap_old = SelectObject(capture_device_context, capture_bitmap);

    BitBlt(
        capture_device_context,
        0, 0,
        MAGNIFIER_SIZE, MAGNIFIER_SIZE,
        screen_device_context,
        x, y,
        SRCCOPY
    );

    BITMAP capture_bitmap_description;
    GetObject(capture_bitmap, sizeof(BITMAP), &capture_bitmap_description);

    BITMAPINFO capture_bitmap_info = {
        .bmiHeader = {
            .biSize = sizeof(BITMAPINFOHEADER),
            .biWidth = capture_bitmap_description.bmWidth,
            .biHeight = -capture_bitmap_description.bmHeight,
            .biPlanes = 1,
            .biBitCount = 32,
            .biCompression = BI_RGB,
        },
    };

    GetDIBits(
        capture_device_context,
        capture_bitmap,
        0,
        capture_bitmap_description.bmHeight,
        recorder->image,
        &capture_bitmap_info,
        DIB_RGB_COLORS
    );

    SelectObject(capture_device_context, capture_bitmap_old);
    DeleteObject(capture_bitmap);
    DeleteDC(capture_device_context);

    ReleaseDC(NULL, screen_device_context);
}

void screen_recorder_snap_via_magnification_api(ScreenRecorder *recorder, int x, int y) {
    recorder->screen_offset_x = x;
    recorder->screen_offset_y = y;

    // I tried sourcing just the rectangle I need to capture, but doing so would cause a tiny amount
    // of jitter while moving the magnifier window around. Sourcing the entire screen and then
    // copying the rectangle of interest inside of the callback seems to fix this problem.
    RECT source_rect = {0, 0, recorder->screen_width, recorder->screen_height};
    MagSetWindowSource(recorder->magnifier_window, source_rect);
}

void screen_recorder_snap(ScreenRecorder *recorder, int x, int y) {
    switch (recorder->method) {
        case SCREEN_RECORDING_VIA_DEVICE_CONTEXT: {
            screen_recorder_snap_via_device_context(recorder, x, y);
        } break;

        case SCREEN_RECORDING_VIA_MAGNIFICATION_API: {
            screen_recorder_snap_via_magnification_api(recorder, x, y);
        } break;
    }
}

typedef struct {
    HWND window_handle;
    HDC device_context;
    HGLRC opengl_context;

    int position_x;
    int position_y;

    int render_width;
    int render_height;
} Magnifier;

PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB;

#define OPENGL_FUNCTIONS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData)

#define X(type, name) type name;
OPENGL_FUNCTIONS(X)
#undef X

bool magnifier_initialize(int render_width, int render_height, Magnifier *magnifier) {
    magnifier->position_x = 0;
    magnifier->position_y = 0;

    magnifier->render_width = render_width;
    magnifier->render_height = render_height;

    HWND dummy_window_handle = CreateWindowEx(
        0,
        L"STATIC",
        L"Dummy window to setup OpenGL context on this stupid operating system",
        WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, NULL, NULL
    );
    HDC dummy_device_context = GetDC(dummy_window_handle);

    PIXELFORMATDESCRIPTOR dummy_pixel_format_descriptor = {
        .nSize = sizeof(dummy_pixel_format_descriptor),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
    };
    int dummy_pixel_format = ChoosePixelFormat(
        dummy_device_context,
        &dummy_pixel_format_descriptor
    );
    if (dummy_pixel_format == 0) {
        return false;
    }

    DescribePixelFormat(
        dummy_device_context,
        dummy_pixel_format,
        sizeof(dummy_pixel_format_descriptor),
        &dummy_pixel_format_descriptor
    );
    if (!SetPixelFormat(dummy_device_context, dummy_pixel_format, &dummy_pixel_format_descriptor)) {
        return false;
    }

    HGLRC dummy_opengl_context = wglCreateContext(dummy_device_context);
    wglMakeCurrent(dummy_device_context, dummy_opengl_context);

    wglChoosePixelFormatARB = (void *)wglGetProcAddress("wglChoosePixelFormatARB");
    wglCreateContextAttribsARB = (void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (wglChoosePixelFormatARB == NULL || wglCreateContextAttribsARB == NULL) {
        return false;
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(dummy_opengl_context);
    ReleaseDC(dummy_window_handle, dummy_device_context);
    DestroyWindow(dummy_window_handle);

    WNDCLASS window_class = {
        .hInstance = GetModuleHandle(NULL),
        .lpszClassName = L"Magnifier::OffscreenRenderWindow",
        .lpfnWndProc = DefWindowProc,
    };
    RegisterClass(&window_class);

    magnifier->window_handle = CreateWindowEx(
        0,
        window_class.lpszClassName,
        L"Magnifier offscreen render",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, window_class.hInstance, NULL
    );
    magnifier->device_context = GetDC(magnifier->window_handle);

    int pixel_format_attributes[] = {
        WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
        WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
        WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_COLOR_BITS_ARB, 24,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_STENCIL_BITS_ARB, 8,
        0,
    };

    int pixel_format;
    UINT pixel_format_count;
    BOOL choose_pixel_format_result = wglChoosePixelFormatARB(
        magnifier->device_context,
        pixel_format_attributes,
        NULL,
        1,
        &pixel_format, &pixel_format_count
    );
    if (!choose_pixel_format_result) {
        return false;
    }

    PIXELFORMATDESCRIPTOR pixel_format_descriptor = {.nSize = sizeof(pixel_format_descriptor)};
    DescribePixelFormat(
        magnifier->device_context,
        pixel_format,
        sizeof(pixel_format_descriptor),
        &pixel_format_descriptor
    );

    if (!SetPixelFormat(magnifier->device_context, pixel_format, &pixel_format_descriptor)) {
        return false;
    }

    int opengl_context_attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };

    magnifier->opengl_context = wglCreateContextAttribsARB(
        magnifier->device_context,
        NULL,
        opengl_context_attributes
    );
    if (magnifier->opengl_context == NULL) {
        return false;
    }

    wglMakeCurrent(magnifier->device_context, magnifier->opengl_context);

    #define X(type, name) name = (type)wglGetProcAddress(#name);
    OPENGL_FUNCTIONS(X)
    #undef X

    char const *vertex_shader_source = SRC(
        \x23version 450 core\n

        layout (location = 0) in vec2 position;

        void main() {
            gl_Position = vec4(position, 0.0, 1.0);
        }
    );

    char const *fragment_shader_source = SRC(
        \x23version 450 core\n

        out vec4 fragment_color;

        void main() {
            fragment_color = vec4(0.0, 0.0, 0.0, 0.375);
        }
    );

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);

    GLint vertex_shader_compiled, fragment_shader_compiled;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &vertex_shader_compiled);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fragment_shader_compiled);
    if (vertex_shader_compiled == 0 || fragment_shader_compiled == 0) {
        return false;
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    GLint shader_program_linked;
    glGetProgramiv(shader_program, GL_LINK_STATUS, &shader_program_linked);
    if (shader_program_linked == 0) {
        return false;
    }

    GLuint framebuffer_texture;
    glGenTextures(1, &framebuffer_texture);
    glBindTexture(GL_TEXTURE_2D, framebuffer_texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        magnifier->render_width, magnifier->render_height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        NULL
    );

    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        framebuffer_texture,
        0
    );

    GLuint vertex_array;
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);

    float rectangle_vertices[] = {
        -1, -1, 1,
        -1, 1, 1,

        -1, -1, 1,
        1, -1, 1,
    };
    GLuint vertex_buffer;
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(rectangle_vertices), rectangle_vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);

    glUseProgram(shader_program);
    glViewport(0, 0, magnifier->render_width, magnifier->render_height);

    return true;
}

void magnifier_render(Magnifier *magnifier, u32 const *background_image, u32 *render_image) {
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(
        0, 0,
        magnifier->render_width, magnifier->render_height,
        GL_BGRA,
        GL_UNSIGNED_BYTE,
        render_image
    );

    for (int y = 0; y < MAGNIFIER_SIZE / 2; y += 1) {
        for (int x = 0; x < MAGNIFIER_SIZE / 2; x += 1) {
            int background_x = x + MAGNIFIER_SIZE / 4;
            int background_y = y + MAGNIFIER_SIZE / 4;

            u32 background = background_image[background_y * MAGNIFIER_SIZE + background_x];
            background |= 0xff000000;

            for (int dy = 0; dy <= 1; dy += 1) {
                for (int dx = 0; dx <= 1; dx += 1) {
                    int render_x = magnifier->position_x + 2 * x + dx;
                    int render_y = magnifier->position_y + 2 * y + dy;

                    if (
                        0 <= render_x && render_x < magnifier->render_width &&
                        0 <= render_y && render_y < magnifier->render_height
                    ) {
                        render_image[render_y * magnifier->render_width + render_x] = background;
                    }
                }
            }
        }
    }
}

LRESULT CALLBACK window_procedure(
    HWND window_handle,
    UINT message,
    WPARAM w_parameter,
    LPARAM l_parameter
) {
    LRESULT result;

    switch (message) {
        case WM_DESTROY: {
            PostQuitMessage(0);
            result = 0;
        } break;

        default: {
            result = DefWindowProc(window_handle, message, w_parameter, l_parameter);
        } break;
    }

    return result;
}

int APIENTRY WinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PSTR command_line,
    int show_command
) {
    SetProcessDPIAware();

    u32 *background_image = malloc(MAGNIFIER_SIZE * MAGNIFIER_SIZE * sizeof(u32));

    ScreenRecorder screen_recorder;
    if (!screen_recorder_create(
        SCREEN_RECORDING_METHOD,
        background_image, MAGNIFIER_SIZE, MAGNIFIER_SIZE,
        &screen_recorder
    )) {
        return 1;
    }

    int render_width = GetSystemMetrics(SM_CXSCREEN);
    int render_height = GetSystemMetrics(SM_CYSCREEN);

    Magnifier magnifier;
    if (!magnifier_initialize(render_width, render_height, &magnifier)) {
        return 1;
    }

    WNDCLASS window_class = {
        .hInstance = instance,
        .lpszClassName = L"Magnifier::RenderWindow",
        .lpfnWndProc = window_procedure,
        .style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW,
        .hCursor = LoadCursor(NULL, IDC_CROSS),
    };
    RegisterClass(&window_class);

    HWND window_handle = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        window_class.lpszClassName,
        L"Magnifier",
        WS_POPUP,
        0, 0,
        render_width, render_height,
        NULL, NULL, window_class.hInstance, NULL
    );

    HBITMAP render_bitmap;
    u32 *render_image;
    {
        BITMAPINFO bitmap_info = {
            .bmiHeader = {
                .biSize = sizeof(BITMAPINFOHEADER),
                .biWidth = render_width,
                .biHeight = -render_height,
                .biPlanes = 1,
                .biSizeImage = render_width * render_height * sizeof(u32),
                .biBitCount = sizeof(u32) * 8,
            },
        };

        HDC window_device_context = GetDC(window_handle);
        render_bitmap = CreateDIBSection(
            window_device_context,
            &bitmap_info,
            DIB_RGB_COLORS,
            (void **)&render_image, NULL, 0
        );
        ReleaseDC(window_handle, window_device_context);
    }

    ShowWindow(window_handle, show_command);
    UpdateWindow(window_handle);

    screen_recorder_exclude_window(&screen_recorder, window_handle);

    bool magnifier_is_dragged = false;

    bool is_running = true;
    while (is_running) {
        MSG message;
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_RBUTTONDOWN || message.message == WM_QUIT) {
                is_running = false;
                continue;
            }

            if (message.message == WM_LBUTTONDOWN) {
                magnifier_is_dragged = true;
                continue;
            }
            if (message.message == WM_LBUTTONUP) {
                magnifier_is_dragged = false;
                continue;
            }

            TranslateMessage(&message);
            DispatchMessage(&message);
        }

        if (magnifier_is_dragged) {
            POINT cursor_position;
            GetCursorPos(&cursor_position);

            magnifier.position_x = cursor_position.x - MAGNIFIER_SIZE / 2;
            magnifier.position_y = cursor_position.y - MAGNIFIER_SIZE / 2;
        }

        // Capture a portion of the screen.

        screen_recorder_snap(&screen_recorder, magnifier.position_x, magnifier.position_y);

        // Magnify the captured image.

        magnifier_render(&magnifier, background_image, render_image);

        // Render magnified image into the layered window.

        HDC window_device_context = GetDC(window_handle);
        HDC render_device_context = CreateCompatibleDC(window_device_context);
        HBITMAP render_bitmap_old = SelectObject(render_device_context, render_bitmap);

        BLENDFUNCTION blend_function = {
            .BlendOp = AC_SRC_OVER,
            .BlendFlags = 0,
            .SourceConstantAlpha = 255,
            .AlphaFormat = AC_SRC_ALPHA,
        };
        UpdateLayeredWindow(
            window_handle,
            NULL,
            NULL,
            &(SIZE){render_width, render_height},
            render_device_context,
            &(POINT){0, 0},
            0,
            &blend_function,
            ULW_ALPHA
        );

        SelectObject(render_device_context, render_bitmap_old);
        DeleteDC(render_device_context);
        ReleaseDC(window_handle, window_device_context);

        GdiFlush();
    }

    return 0;
}
