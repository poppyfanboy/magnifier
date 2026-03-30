#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef uint32_t u32;

#define MAGNIFIER_SIZE 256

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
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        NULL,
        NULL,
        instance,
        NULL
    );
    if (window_handle == NULL) {
        return 1;
    }

    u32 *captured_image = malloc(MAGNIFIER_SIZE * MAGNIFIER_SIZE * sizeof(u32));

    int render_width = GetSystemMetrics(SM_CXSCREEN);
    int render_height = GetSystemMetrics(SM_CYSCREEN);

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
            (void **)&render_image,
            NULL,
            0
        );
        ReleaseDC(window_handle, window_device_context);
    }

    SetWindowDisplayAffinity(window_handle, WDA_EXCLUDEFROMCAPTURE);

    ShowWindow(window_handle, show_command);
    UpdateWindow(window_handle);

    bool magnifier_is_dragged = false;
    POINT magnifier_position = {0, 0};

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
            GetCursorPos(&magnifier_position);
            magnifier_position.x -= MAGNIFIER_SIZE / 2;
            magnifier_position.y -= MAGNIFIER_SIZE / 2;
        }

        HDC screen_device_context = GetDC(NULL);
        HDC window_device_context = GetDC(window_handle);

        // Capture a portion of the screen.

        HDC capture_device_context = CreateCompatibleDC(screen_device_context);
        HBITMAP capture_bitmap = CreateCompatibleBitmap(
            screen_device_context,
            MAGNIFIER_SIZE,
            MAGNIFIER_SIZE
        );
        HBITMAP capture_bitmap_old = SelectObject(capture_device_context, capture_bitmap);

        BitBlt(
            capture_device_context,
            0,
            0,
            MAGNIFIER_SIZE,
            MAGNIFIER_SIZE,
            screen_device_context,
            magnifier_position.x,
            magnifier_position.y,
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
            captured_image,
            &capture_bitmap_info,
            DIB_RGB_COLORS
        );

        SelectObject(capture_device_context, capture_bitmap_old);
        DeleteObject(capture_bitmap);
        DeleteDC(capture_device_context);

        // Magnify the captured image.

        memset(render_image, 0, render_width * render_height * sizeof(u32));

        for (int y = 0; y < MAGNIFIER_SIZE / 2; y += 1) {
            for (int x = 0; x < MAGNIFIER_SIZE / 2; x += 1) {
                int captured_x = x + MAGNIFIER_SIZE / 4;
                int captured_y = y + MAGNIFIER_SIZE / 4;

                u32 captured_pixel = captured_image[captured_y * MAGNIFIER_SIZE + captured_x];
                captured_pixel |= 0xff000000;

                for (int dy = 0; dy <= 1; dy += 1) {
                    for (int dx = 0; dx <= 1; dx += 1) {
                        int render_x = magnifier_position.x + 2 * x + dx;
                        int render_y = magnifier_position.y + 2 * y + dy;

                        if (
                            0 <= render_x && render_x < render_width &&
                            0 <= render_y && render_y < render_height
                        ) {
                            render_image[render_y * render_width + render_x] = captured_pixel;
                        }
                    }
                }
            }
        }

        // Render magnified image into the layered window.

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

        BitBlt(
            window_device_context,
            0,
            0,
            render_width,
            render_height,
            render_device_context,
            0,
            0,
            SRCCOPY
        );

        SelectObject(render_device_context, render_bitmap_old);
        DeleteDC(render_device_context);

        ReleaseDC(window_handle, window_device_context);
        ReleaseDC(NULL, screen_device_context);
        GdiFlush();
    }

    return 0;
}
