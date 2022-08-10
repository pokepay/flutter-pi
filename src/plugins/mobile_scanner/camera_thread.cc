// clang-format off
#include "camera_thread.h"
extern "C"{
#include "texture_registry.h"
}
#include <atomic>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <GLES2/gl2.h>
// clang-format on

#include <stdio.h>
#define FILE_DESCR(_logging_name) static const char *__file_logging_name = _logging_name;
#define LOG_ERROR(fmtstring, ...) fprintf(stderr, "[%s] " fmtstring, __file_logging_name, ##__VA_ARGS__)
#define LOG_DEBUG(fmtstring, ...) fprintf(stderr, "[%s] " fmtstring, __file_logging_name, ##__VA_ARGS__)

FILE_DESCR("MobileScanner")

typedef struct CameraThreadState
{
    cv::VideoCapture *cap;
    int device_id;
    std::atomic_bool running;
    struct texture *texture;
} CameraThreadState;

extern "C" CameraThreadState *CameraThreadState_new()
{
    auto self = new CameraThreadState();
    self->cap = nullptr;
    self->device_id = 0;
    self->running.store(false);
    return self;
}

extern "C" CameraThreadResult CameraThreadState_init(CameraThreadState *self, int device_id)
{
    self->device_id = device_id;
    self->cap = new cv::VideoCapture(device_id);
    if (self->cap->isOpened()) {
        return CAMERA_THREAD_SUCCESS;
    }

    LOG_ERROR("Could not open camera device\n");
    delete self->cap;
    self->cap = nullptr;
    return CAMERA_THREAD_FAILURE;
}

extern "C" double CameraThreadState_getWidth(CameraThreadState *self)
{
    return self->cap->get(cv::CAP_PROP_FRAME_WIDTH);
}

extern "C" double CameraThreadState_getHeight(CameraThreadState *self)
{
    return self->cap->get(cv::CAP_PROP_FRAME_HEIGHT);
}

static void on_destroy_texture_frame(const struct texture_frame *texture_frame, void *userdata)
{
    (void)texture_frame;
    (void)userdata;
    // TODO
}

extern "C" void *camera_thread_main(void *arg)
{
    LOG_DEBUG("camera thread starting\n");
    cv::Mat frame;
    auto state = (CameraThreadState *)arg;
    state->running.store(true);
    while (state->running.load()) {
        state->cap->read(frame);
        if (frame.empty()) {
            // May exit because the camera got disconnected
            // TODO: signal error some way?
            state->running.store(false);
            break;
        }

        struct gl_texture_frame gl_frame;
        // TODO fill me in
        // glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        // glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glGenTextures(1, &gl_frame.name);
        glBindTexture(GL_TEXTURE_2D, gl_frame.name);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Set texture clamping method
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        cv::cvtColor(frame, frame, cv::COLOR_RGB2BGR);

        glTexImage2D(
          GL_TEXTURE_2D,    // Type of texture
          0,                // Pyramid level (for mip-mapping) - 0 is the top level
          GL_RGB,           // Internal colour format to convert to
          frame.cols,       // Image width  i.e. 640 for Kinect in standard mode
          frame.rows,       // Image height i.e. 480 for Kinect in standard mode
          0,                // Border width in pixels (can either be 1 or 0)
          GL_RGB,           // Input image format (i.e. GL_RGB, GL_RGBA, GL_BGR etc.)
          GL_UNSIGNED_BYTE, // Image data type
          frame.ptr());     // The actual image data itself

        struct texture_frame texture_frame;
        texture_frame.gl = gl_frame;
        texture_frame.destroy = on_destroy_texture_frame;
        texture_frame.userdata = NULL;

        texture_push_frame(state->texture, &texture_frame);
    }

    state->cap->release();
    delete state->cap;
    state->cap = nullptr;
    // NOTE do we need to do some other cleanup step here?
    return NULL;
}

extern "C" void CameraThreadState_clean(CameraThreadState *self)
{
    if (self->cap != nullptr) {
        self->cap->release();
        delete self->cap;
        self->cap = nullptr;
    }
    if (self->texture != NULL) {
        texture_destroy(self->texture);
        self->texture = NULL;
    }
}

extern "C" void CameraThreadState_delete(CameraThreadState *self)
{
    delete self;
}
